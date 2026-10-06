#include <fcntl.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/syslog_sink.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <string>

#include "cli/auth_command.h"
#include "cli/daemon_commands.h"
#include "cli/probe_command.h"

namespace {

// This process's stdout/stderr are inherited from whatever spawned the PAM
// stack, and PAM callers routinely treat those fds as a structured control
// channel rather than a log stream -- e.g. GNOME Shell's polkit auth agent
// reads polkit-agent-helper-1's inherited output as a strict line protocol
// (SUCCESS / FAILURE / PAM_PROMPT_ECHO_OFF ...). linux-hello-camera-helper is
// forked from that same process tree, so *any* line it writes there, even a
// single warning, is garbage to that parser and derails the caller's state
// machine (observed as GNOME Shell logging "Unknown line ... from helper" and
// retrying authentication in a tight loop -- `pkexec id` never returning).
// So: `auth` never writes to stdout/stderr, regardless of level. Point both at
// /dev/null before anything else runs (even argument parsing and ONNX Runtime
// warnings) and log to syslog only.
void silenceStdio() {
  const int devnull = open("/dev/null", O_RDWR);
  if (devnull < 0) {
    return;
  }
  dup2(devnull, STDIN_FILENO);
  dup2(devnull, STDOUT_FILENO);
  dup2(devnull, STDERR_FILENO);
  if (devnull > STDERR_FILENO) {
    close(devnull);
  }
}

void logToSyslog() {
  auto logger =
      spdlog::syslog_logger_mt("linux-hello-camera", "linux-hello-camera", LOG_PID, LOG_AUTHPRIV);
  logger->set_pattern("%v");
  spdlog::set_default_logger(logger);
  spdlog::set_level(spdlog::level::info);
}

// The other commands print their result on stdout, so diagnostics go to stderr.
void logToStderr() {
  auto logger = spdlog::stderr_color_mt("linux-hello-camera");
  logger->set_pattern("%^%l%$: %v");
  spdlog::set_default_logger(logger);
  spdlog::set_level(spdlog::level::info);
}

}  // namespace

int main(int argc, char** argv) {
  const bool is_auth = argc > 1 && strcmp(argv[1], "auth") == 0;
  if (is_auth) {
    silenceStdio();
    logToSyslog();
  } else {
    logToStderr();
  }

  CLI::App app{"Linux Hello Camera helper"};
  app.set_version_flag("--version,-v", LHC_VERSION);
  app.require_subcommand(1, 1);

  // Everything that touches face data is a request to the daemon (DESIGN.md).
  std::string username, service;
  bool remote = false;
  auto auth_cmd = app.add_subcommand("auth", "Authenticate a user (run by the PAM module)");
  auth_cmd->add_option("--username,-u", username, "User to authenticate")->required();
  auth_cmd->add_option("--service,-s", service, "PAM service name");
  auth_cmd->add_flag("--remote", remote, "The PAM caller reported a remote host (PAM_RHOST)");

  std::string test_user;
  auto test_cmd = app.add_subcommand("test", "Try a face match and print the result as JSON");
  test_cmd->add_option("--username,-u", test_user, "User to match")->required();

  std::string enroll_user, enroll_face, enroll_name;
  int enroll_count = 5;
  auto enroll_cmd = app.add_subcommand("enroll", "Add face templates for a user (root only)");
  enroll_cmd->add_option("--username,-u", enroll_user, "User to enroll")->required();
  enroll_cmd->add_option("--count,-n", enroll_count, "Faces to capture")
      ->check(CLI::Range(1, lhc::kMaxEnrollCount));
  auto enroll_face_opt = enroll_cmd->add_option(
      "--face", enroll_face, "Add to this face from `list` (default: a new one)");
  enroll_cmd->add_option("--name", enroll_name, "Name of the new face")->excludes(enroll_face_opt);

  std::string rename_user, rename_face, rename_name;
  auto rename_cmd = app.add_subcommand("rename", "Name an enrolled face (root only)");
  rename_cmd->add_option("--username,-u", rename_user, "User")->required();
  rename_cmd->add_option("--face", rename_face, "Face id from `list`")->required();
  rename_cmd->add_option("--name", rename_name, "The new name")->required();

  std::string list_user;
  auto list_cmd = app.add_subcommand("list", "Print a user's enrolled faces as JSON");
  list_cmd->add_option("--username,-u", list_user, "User to list")->required();

  std::string remove_user, remove_id, remove_face;
  auto remove_cmd =
      app.add_subcommand("remove", "Delete one face picture, or a whole face (root only)");
  remove_cmd->add_option("--username,-u", remove_user, "User")->required();
  auto remove_id_opt = remove_cmd->add_option("--id", remove_id, "Entry id from `list`");
  auto remove_face_opt =
      remove_cmd->add_option("--face", remove_face, "Face id from `list`: all its pictures");
  remove_id_opt->excludes(remove_face_opt);
  remove_cmd->require_option(1);

  std::string clear_user;
  auto clear_cmd = app.add_subcommand("clear", "Delete all of a user's faces (root only)");
  clear_cmd->add_option("--username,-u", clear_user, "User")->required();

  auto migrate_cmd =
      app.add_subcommand("migrate", "Turn 0.8 face crops into templates and delete them (root)");

  std::string encryption_value;
  auto encryption_cmd =
      app.add_subcommand("set-encryption", "Turn TPM encryption of the templates on or off (root)");
  encryption_cmd->add_option("value", encryption_value, "on, off or auto")
      ->required()
      ->check(CLI::IsMember({"on", "off", "auto"}));

  auto status_cmd = app.add_subcommand("status", "Print TPM / Secure Boot / encryption state");

  lhc::ProbeOptions probe;
  auto probe_cmd = app.add_subcommand("probe", "Print per-frame detection and IR liveness numbers");
  std::string probe_dir;
  auto probe_dir_cmd =
      app.add_subcommand("probe-dir", "Like probe, from a recorded session folder");
  probe_dir_cmd->add_option("dir", probe_dir, "Folder with session.json and PNG frames")
      ->required()
      ->check(CLI::ExistingDirectory);
  for (CLI::App* cmd : {probe_cmd, probe_dir_cmd}) {
    cmd->add_option("--config", probe.config_path, "Config file to use instead of the system one");
    cmd->add_option("--faces", probe.faces_dir, "Also score against the PNG crops in this folder")
        ->check(CLI::ExistingDirectory);
  }
  probe_cmd->add_option("--seconds", probe.seconds, "How long to capture")
      ->check(CLI::PositiveNumber);
  probe_cmd->add_option("--camera,-c", probe.camera, "Camera device (default: from the config)");
  probe_dir_cmd
      ->add_option("--save-crops", probe.save_crops,
                   "Write the crops enroll would pick to this folder")
      ->check(CLI::ExistingDirectory);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  const lhc::ClientContext context;
  auto request = [](lhc::Cmd cmd, const std::string& user) {
    lhc::Request r;
    r.cmd = cmd;
    r.username = user;
    return r;
  };

  if (app.got_subcommand(auth_cmd)) {
    return lhc::runAuthCommand(context, username, service, remote);
  }
  if (app.got_subcommand(test_cmd)) {
    return lhc::runTestCommand(context, test_user);
  }
  if (app.got_subcommand(enroll_cmd)) {
    return lhc::runEnrollCommand(context, enroll_user, enroll_count, enroll_face, enroll_name);
  }
  if (app.got_subcommand(list_cmd)) {
    return lhc::runQueryCommand(context, request(lhc::Cmd::kList, list_user));
  }
  if (app.got_subcommand(status_cmd)) {
    return lhc::runQueryCommand(context, request(lhc::Cmd::kStatus, ""));
  }
  if (app.got_subcommand(remove_cmd)) {
    lhc::Request r = request(lhc::Cmd::kRemove, remove_user);
    r.id = remove_id;
    r.face = remove_face;
    return lhc::runChangeCommand(context, r);
  }
  if (app.got_subcommand(rename_cmd)) {
    lhc::Request r = request(lhc::Cmd::kRename, rename_user);
    r.face = rename_face;
    r.name = rename_name;
    return lhc::runChangeCommand(context, r);
  }
  if (app.got_subcommand(clear_cmd)) {
    return lhc::runChangeCommand(context, request(lhc::Cmd::kClear, clear_user));
  }
  if (app.got_subcommand(migrate_cmd)) {
    return lhc::runChangeCommand(context, request(lhc::Cmd::kMigrate, ""));
  }
  if (app.got_subcommand(encryption_cmd)) {
    lhc::Request r = request(lhc::Cmd::kSetEncryption, "");
    r.value = encryption_value;
    return lhc::runChangeCommand(context, r);
  }
  if (app.got_subcommand(probe_cmd)) {
    return lhc::runProbe(probe);
  }
  return lhc::runProbeDir(probe_dir, probe);
}
