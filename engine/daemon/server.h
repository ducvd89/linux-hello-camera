#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <string>

#include "daemon/authorization.h"
#include "daemon/engine.h"

namespace lhc {

struct ServerOptions {
  int listen_fd = -1;       // an already listening socket (socket activation), or
  std::string socket_path;  // a path to bind (started by hand); exactly one is used
  int idle_timeout_s = 60;  // exit after this long without a request; 0 = never
  uid_t root_uid = 0;       // the peer uid with full rights; tests use their own uid
  UidLookup uid_lookup = systemUidLookup;
};

// Accepts connections on a Unix socket, checks each peer with SO_PEERCRED and hands requests to
// the Engine, one thread per connection.
class Server {
 public:
  Server(Engine& engine, ServerOptions options) : engine_(engine), options_(std::move(options)) {}
  ~Server();

  // Binds or adopts the socket. False with `error` on failure.
  bool start(std::string& error);

  // Serves until idle (no connection for idle_timeout_s and no recent failed attempts) or until
  // stop() is called. Returns after all connections finished.
  void run();

  // Makes run() return; safe from a signal handler.
  void stop();

 private:
  void serve(int fd);

  Engine& engine_;
  ServerOptions options_;
  int listen_fd_ = -1;
  bool owns_path_ = false;
  int wake_[2] = {-1, -1};
  std::atomic<int> active_{0};
  std::atomic<bool> stopping_{false};
};

// The listening socket systemd passed us (LISTEN_PID/LISTEN_FDS), or -1 if we were not
// socket-activated. Clears the variables so children do not inherit them.
int listenFdFromSystemd();

// Pure parsing part of listenFdFromSystemd(), for tests: -1 unless `pid` matches and fds >= 1.
int parseListenFds(const char* listen_pid, const char* listen_fds, pid_t our_pid);

}  // namespace lhc
