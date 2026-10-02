#include "daemon/server.h"

#include <fcntl.h>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace lhc {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSdListenFdsStart = 3;
constexpr int kRequestReadTimeoutS = 5;

bool writeAll(int fd, const std::string& data) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += static_cast<size_t>(n);
  }
  return true;
}

// One line, without its newline; false on timeout, EOF before a newline, or an over-long line.
bool readLine(int fd, std::string& line) {
  line.clear();
  char buf[512];
  while (line.size() <= kMaxRequestBytes) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    line.append(buf, static_cast<size_t>(n));
    const size_t eol = line.find('\n');
    if (eol != std::string::npos) {
      line.resize(eol);
      return true;
    }
  }
  return false;
}

}  // namespace

int parseListenFds(const char* listen_pid, const char* listen_fds, pid_t our_pid) {
  if (listen_pid == nullptr || listen_fds == nullptr) {
    return -1;
  }
  char* end = nullptr;
  const long pid = std::strtol(listen_pid, &end, 10);
  if (end == listen_pid || *end != '\0' || pid != our_pid) {
    return -1;
  }
  const long fds = std::strtol(listen_fds, &end, 10);
  if (end == listen_fds || *end != '\0' || fds < 1) {
    return -1;
  }
  return kSdListenFdsStart;
}

int listenFdFromSystemd() {
  const int fd = parseListenFds(std::getenv("LISTEN_PID"), std::getenv("LISTEN_FDS"), getpid());
  unsetenv("LISTEN_PID");
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
  if (fd < 0) {
    return -1;
  }
  int listening = 0;
  socklen_t len = sizeof(listening);
  if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &len) != 0 || !listening) {
    return -1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

Server::~Server() {
  if (listen_fd_ >= 0)
    close(listen_fd_);
  if (owns_path_)
    unlink(options_.socket_path.c_str());
  for (int fd : wake_) {
    if (fd >= 0)
      close(fd);
  }
}

bool Server::start(std::string& error) {
  if (pipe2(wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    return false;
  }
  if (options_.listen_fd >= 0) {
    listen_fd_ = options_.listen_fd;
    return true;
  }

  const std::string& path = options_.socket_path;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
    error = "bad socket path";
    return false;
  }
  std::strcpy(addr.sun_path, path.c_str());

  const size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) {
    mkdir(path.substr(0, slash).c_str(), 0755);
  }
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  unlink(path.c_str());  // a stale socket from an earlier run
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      chmod(path.c_str(), 0666) != 0 || listen(listen_fd_, 16) != 0) {
    error = "cannot listen on " + path + ": " + std::strerror(errno);
    return false;
  }
  owns_path_ = true;
  return true;
}

void Server::stop() {
  stopping_ = true;
  const char byte = 0;
  if (wake_[1] >= 0) {
    ssize_t ignored = write(wake_[1], &byte, 1);
    (void)ignored;
  }
}

void Server::run() {
  auto last_activity = Clock::now();
  while (!stopping_) {
    pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
    const int ready = poll(fds, 2, 1000);
    if (ready < 0 && errno != EINTR) {
      break;
    }
    if (ready > 0 && (fds[0].revents & POLLIN)) {
      const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) {
        last_activity = Clock::now();
        active_++;
        std::thread([this, fd] {
          serve(fd);
          close(fd);
          active_--;
        }).detach();
      }
    }

    const auto now = Clock::now();
    if (active_ > 0) {
      last_activity = now;
    } else if (options_.idle_timeout_s > 0 &&
               now - last_activity >= std::chrono::seconds(options_.idle_timeout_s) &&
               !engine_.hasRecentFailures()) {
      spdlog::info("idle, exiting");
      break;
    }
  }
  while (active_ > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void Server::serve(int fd) {
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    return;
  }
  // A client that connects and says nothing must not hold a thread forever.
  timeval tv{kRequestReadTimeoutS, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::string line, error;
  Request request;
  if (!readLine(fd, line)) {
    return;
  }
  if (!parseRequest(line, request, error)) {
    writeAll(fd, toLine(errorReply(error)));
    return;
  }
  if (!isAuthorized(request, cred.uid, options_.root_uid, options_.uid_lookup)) {
    spdlog::warn("refused request from uid {} (pid {})", cred.uid, cred.pid);
    writeAll(
        fd,
        toLine(errorReply(request.cmd == Cmd::kEnroll || request.cmd == Cmd::kTest ||
                                  request.cmd == Cmd::kClear || request.cmd == Cmd::kRemove ||
                                  request.cmd == Cmd::kMigrate || request.cmd == Cmd::kSetEncryption
                              ? "not root"
                              : "not allowed")));
    return;
  }

  // Long requests (enroll) may take a while; only the request itself had a read deadline.
  timeval none{0, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));

  const nlohmann::json reply = engine_.handle(
      request, [fd](const nlohmann::json& line_json) { return writeAll(fd, toLine(line_json)); });
  writeAll(fd, toLine(reply));
}

}  // namespace lhc
