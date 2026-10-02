#include "cli/daemon_client.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace lhc {

namespace {

using Clock = std::chrono::steady_clock;

int msUntil(Clock::time_point deadline) {
  const auto left =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return left < 0 ? 0 : static_cast<int>(left);
}

// A connected socket, or -1. A full backlog makes a non-blocking connect() fail with EAGAIN, so
// retry briefly until the deadline.
int connectTo(const std::string& path, Clock::time_point deadline) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    return -1;
  }
  std::strcpy(addr.sun_path, path.c_str());

  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    return -1;
  }
  for (;;) {
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      return fd;
    }
    if (errno != EAGAIN || msUntil(deadline) == 0) {
      close(fd);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool sendAll(int fd, const std::string& data, Clock::time_point deadline) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
    if (n > 0) {
      done += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && errno == EAGAIN) {
      pollfd p{fd, POLLOUT, 0};
      if (poll(&p, 1, msUntil(deadline)) <= 0)
        return false;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

CallResult callDaemon(const ClientContext& context, const Request& request, int read_timeout_ms,
                      const std::function<void(const nlohmann::json&)>& on_progress) {
  CallResult result;
  const int fd =
      connectTo(context.socket_path, Clock::now() + std::chrono::milliseconds(kConnectTimeoutMs));
  if (fd < 0) {
    result.error = "daemon unreachable";
    return result;
  }
  result.status = CallStatus::kBroken;
  const auto deadline = Clock::now() + std::chrono::milliseconds(read_timeout_ms);

  if (!sendAll(fd, requestToJson(request) + "\n", deadline)) {
    result.error = "cannot send request";
    close(fd);
    return result;
  }

  std::string buffer;
  for (;;) {
    size_t eol;
    while ((eol = buffer.find('\n')) != std::string::npos) {
      const std::string line = buffer.substr(0, eol);
      buffer.erase(0, eol + 1);
      nlohmann::json doc = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
      if (!doc.is_object()) {
        result.error = "bad reply from daemon";
        close(fd);
        return result;
      }
      if (doc.contains("progress") && !doc.contains("result")) {
        if (on_progress)
          on_progress(doc);
        continue;
      }
      result.reply = std::move(doc);
      result.status = CallStatus::kOk;
      close(fd);
      return result;
    }

    pollfd p{fd, POLLIN, 0};
    const int ready = poll(&p, 1, msUntil(deadline));
    if (ready == 0) {
      result.error = "daemon did not answer in time";
      break;
    }
    if (ready < 0 && errno == EINTR)
      continue;
    char buf[1024];
    const ssize_t n = ready < 0 ? -1 : recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      buffer.append(buf, static_cast<size_t>(n));
    } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
      continue;
    } else {
      result.error = "daemon closed the connection";
      break;
    }
  }
  close(fd);
  return result;
}

}  // namespace lhc
