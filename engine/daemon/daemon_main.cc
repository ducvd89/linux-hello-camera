#include "daemon/daemon_main.h"

#include <fcntl.h>
#include <signal.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/syslog_sink.h>
#include <spdlog/spdlog.h>
#include <syslog.h>
#include <unistd.h>

#include "store/template_store.h"

namespace lhc {

namespace {

Server* g_server = nullptr;

void onSignal(int) {
  if (g_server != nullptr) {
    g_server->stop();
  }
}

// Like the helper's `auth`: when systemd or PAM callers own our stdio it is not ours to write to.
void silenceStdio() {
  const int devnull = open("/dev/null", O_RDWR);
  if (devnull < 0)
    return;
  dup2(devnull, STDIN_FILENO);
  dup2(devnull, STDOUT_FILENO);
  dup2(devnull, STDERR_FILENO);
  if (devnull > STDERR_FILENO)
    close(devnull);
}

}  // namespace

int runDaemon(const DaemonOptions& options, Crypto& crypto, CameraOpener open_camera) {
  if (options.log_stderr) {
    auto logger = spdlog::stderr_color_mt("linux-hello-camerad");
    logger->set_pattern("%^%l%$: %v");
    spdlog::set_default_logger(logger);
  } else {
    silenceStdio();
    auto logger = spdlog::syslog_logger_mt("linux-hello-camerad", "linux-hello-camera", LOG_PID,
                                           LOG_AUTHPRIV);
    logger->set_pattern("%v");
    spdlog::set_default_logger(logger);
  }
  spdlog::set_level(spdlog::level::info);
  signal(SIGPIPE, SIG_IGN);

  // Under systemd the socket is already listening; by hand we bind our own.
  ServerOptions server_options;
  server_options.idle_timeout_s = options.idle_timeout_s;
  server_options.root_uid = options.root_uid;
  if (options.socket_path.empty()) {
    server_options.listen_fd = listenFdFromSystemd();
    if (server_options.listen_fd < 0) {
      spdlog::error("not socket-activated and no --socket given");
      return 1;
    }
  } else {
    server_options.socket_path = options.socket_path;
  }

  TemplateStore store(options.templates_dir, crypto);
  Engine engine(options.engine, store, std::move(open_camera));
  Server server(engine, server_options);
  std::string error;
  if (!server.start(error)) {
    spdlog::error("{}", error);
    return 1;
  }

  g_server = &server;
  struct sigaction sa{};
  sa.sa_handler = onSignal;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  engine.reconcileAll();
  spdlog::info("started");
  server.run();
  g_server = nullptr;
  return 0;
}

}  // namespace lhc
