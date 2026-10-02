#pragma once

#include <string>

#include "daemon/engine.h"
#include "daemon/server.h"
#include "store/crypto.h"

namespace lhc {

struct DaemonOptions {
  EngineOptions engine;
  std::string templates_dir;
  std::string socket_path;  // empty: use the socket systemd passed
  int idle_timeout_s = 60;
  uid_t root_uid = 0;
  bool log_stderr = false;  // otherwise syslog only: the daemon never writes to stdout/stderr
};

// The daemon: logging, stdio, socket, signal handling, startup reconcile, then serve until idle or
// SIGTERM. Returns the process exit code. Production and test mains differ only in the options,
// the Crypto and the camera opener they pass.
int runDaemon(const DaemonOptions& options, Crypto& crypto, CameraOpener open_camera);

}  // namespace lhc
