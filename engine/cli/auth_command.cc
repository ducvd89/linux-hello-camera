#include "cli/auth_command.h"

#include <dirent.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "cli/confirm_hook.h"
#include "cli/daemon_client.h"
#include "core/config.h"

namespace lhc {

namespace {

// True if any ACPI lid reports "closed" (the camera then only sees the inside of the lid).
bool lidClosed() {
  const std::string root = "/proc/acpi/button/lid";
  DIR* dir = opendir(root.c_str());
  if (dir == nullptr) {
    return false;
  }
  bool closed = false;
  while (const dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    std::ifstream state(root + "/" + entry->d_name + "/state");
    std::string line;
    while (std::getline(state, line)) {
      closed = closed || line.find("closed") != std::string::npos;
    }
  }
  closedir(dir);
  return closed;
}

// True if the session came in over the network: sudo/su over SSH inherit these variables from
// the SSH session, and the PAM module reports a remote host separately (`--remote`). Without this
// check the camera would match whoever sits at the laptop while someone else types the command.
bool remoteSession(bool pam_remote) {
  if (pam_remote) {
    return true;
  }
  for (const char* name : {"SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0') {
      return true;
    }
  }
  return false;
}

// argv of a process from /proc/<pid>/cmdline; empty if it is gone or unreadable.
std::vector<std::string> commandLine(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::vector<std::string> args;
  std::string arg;
  while (std::getline(file, arg, '\0')) {
    args.push_back(arg);
  }
  return args;
}

int ignore(const std::string& username, const std::string& reason) {
  spdlog::info("user={} ignored: {}", username, reason);
  return kExitIgnore;
}

}  // namespace

bool appAdminRequestPending(const std::string& proc_root, const std::string& settings_helper) {
  DIR* dir = opendir(proc_root.c_str());
  if (dir == nullptr) {
    return false;
  }
  bool pending = false;
  while (const dirent* entry = readdir(dir)) {
    const std::string pid = entry->d_name;
    if (pid.empty() || !std::all_of(pid.begin(), pid.end(), ::isdigit)) {
      continue;
    }
    const std::vector<std::string> args = commandLine(proc_root + "/" + pid + "/cmdline");
    if (args.empty() || args[0].substr(args[0].rfind('/') + 1) != "pkexec") {
      continue;
    }
    // pkexec [--options...] PROGRAM [ARGS...]
    const auto program = std::find_if(args.begin() + 1, args.end(),
                                       [](const std::string& a) { return a.rfind("--", 0) != 0; });
    if (program != args.end() && *program == settings_helper) {
      pending = true;
      break;
    }
  }
  closedir(dir);
  return pending;
}

int runAuthCommand(const ClientContext& context, const std::string& username,
                   const std::string& service, bool remote) {
  Config config;
  std::string error;
  if (!loadConfig(context.config_path, config, error)) {
    return ignore(username, "no usable config");
  }
  if (config.disabled) {
    return ignore(username, "disabled");
  }
  if (!service.empty() && std::find(config.ignore_services.begin(), config.ignore_services.end(),
                                    service) != config.ignore_services.end()) {
    return ignore(username, "service ignored");
  }
  if (config.abort_if_ssh && remoteSession(remote)) {
    return ignore(username, "remote session");
  }
  // The app's own admin requests take the password: see appAdminRequestPending.
  if (service == "polkit-1" && appAdminRequestPending(context.proc_root, context.settings_helper)) {
    return ignore(username, "the app asks for admin rights: password only");
  }
  // The lid is checked here, not in the daemon: its sandbox hides /proc/acpi.
  if (config.abort_if_lid_closed && lidClosed()) {
    return ignore(username, "lid closed");
  }

  Request request;
  request.cmd = Cmd::kAuth;
  request.username = username;
  request.service = service;
  request.remote = remote;
  // The daemon may have to start and load its models, and may make us wait for the camera.
  const CallResult call = callDaemon(context, request, config.timeout_ms + 5000 + 20000);
  if (call.status != CallStatus::kOk) {
    return ignore(username, call.error);
  }

  const std::string result = call.reply.value("result", "");
  if (result == "ignore" || result == "busy") {
    return ignore(username,
                  result == "busy" ? "daemon busy" : call.reply.value("detail", "ignored"));
  }
  spdlog::info("user={} service={} result={} best_score={:.3f} pairs={} elapsed_ms={}", username,
               service, result, call.reply.value("best_score", 0.0),
               call.reply.value("liveness_pairs", 0), call.reply.value("elapsed_ms", 0));
  if (result != "ok") {
    return kExitNoMatch;
  }

  const bool confirm = config.confirm.enabled && !service.empty() &&
                       std::find(config.confirm.services.begin(), config.confirm.services.end(),
                                 service) != config.confirm.services.end();
  if (confirm && !runConfirmHook(context.confirm_hook, username, service)) {
    spdlog::info("user={} service={} confirmation refused", username, service);
    return kExitNoMatch;
  }
  return kExitMatch;
}

}  // namespace lhc
