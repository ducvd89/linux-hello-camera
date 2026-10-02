#include "cli/daemon_commands.h"

#include "cli/output.h"

namespace lhc {

namespace {

// Enough for the daemon to start (models load), wait out a busy camera and run its own session.
constexpr int kSlackMs = 20000;
constexpr int kBusyWaitMs = 5000;
constexpr int kEnrollMs = 20000;
constexpr int kMigrateMs = 60000;

nlohmann::json unreachableReply(const CallResult& call) {
  return {{"result", "error"}, {"detail", call.error}};
}

bool resultIs(const nlohmann::json& reply, const char* value) {
  return reply.is_object() && reply.value("result", "") == value;
}

}  // namespace

int runTestCommand(const ClientContext& context, const std::string& username) {
  Request request;
  request.cmd = Cmd::kTest;
  request.username = username;
  Config config;
  std::string ignored;
  loadConfig(context.config_path, config, ignored);

  const CallResult call = callDaemon(context, request, config.timeout_ms + kBusyWaitMs + kSlackMs);
  const nlohmann::json reply = call.status == CallStatus::kOk ? call.reply : unreachableReply(call);
  printLine(reply.dump());
  return resultIs(reply, "ok") ? 0 : 1;
}

int runEnrollCommand(const ClientContext& context, const std::string& username, int count) {
  Request request;
  request.cmd = Cmd::kEnroll;
  request.username = username;
  request.count = count;

  const CallResult call = callDaemon(
      context, request, kEnrollMs + kBusyWaitMs + kSlackMs, [](const nlohmann::json& progress) {
        printLine("PROGRESS " + std::to_string(progress.value("progress", 0)) + " " +
                  std::to_string(progress.value("total", 0)));
      });
  if (call.status != CallStatus::kOk) {
    printLine("ERR " + call.error);
    return 1;
  }
  if (resultIs(call.reply, "ok")) {
    printLine("OK " + std::to_string(call.reply.value("added", 0)));
    return 0;
  }
  printLine("ERR " + call.reply.value("detail", call.reply.value("result", "failed")));
  return 1;
}

int runQueryCommand(const ClientContext& context, const Request& request) {
  const CallResult call = callDaemon(context, request, kSlackMs);
  if (call.status != CallStatus::kOk) {
    printLine(unreachableReply(call).dump());
    return 1;
  }
  printLine(call.reply.dump());
  return 0;
}

int runChangeCommand(const ClientContext& context, const Request& request) {
  const CallResult call =
      callDaemon(context, request, request.cmd == Cmd::kMigrate ? kMigrateMs : kSlackMs);
  const nlohmann::json reply = call.status == CallStatus::kOk ? call.reply : unreachableReply(call);
  printLine(reply.dump());
  return resultIs(reply, "ok") ? 0 : 1;
}

}  // namespace lhc
