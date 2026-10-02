#include "store/process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

namespace lhc {

ProcessResult runProcess(const std::string& path, const std::vector<std::string>& args,
                         const std::string& input, int timeout_ms) {
  ProcessResult result;
  // Writing to a child that exited must fail with EPIPE, not kill us.
  signal(SIGPIPE, SIG_IGN);

  // Everything the child needs is prepared before fork(): it may only call async-signal-safe
  // functions until exec.
  std::vector<char*> argv;
  std::string name = path.substr(path.rfind('/') + 1);
  argv.push_back(name.data());
  std::vector<std::string> arg_copy = args;
  for (std::string& a : arg_copy) argv.push_back(a.data());
  argv.push_back(nullptr);
  char* envp[] = {nullptr};

  int in_pipe[2], out_pipe[2], err_pipe[2];
  if (pipe2(in_pipe, O_CLOEXEC) != 0)
    return result;
  if (pipe2(out_pipe, O_CLOEXEC) != 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    return result;
  }
  if (pipe2(err_pipe, O_CLOEXEC) != 0) {
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) close(fd);
    return result;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
      close(fd);
    }
    return result;
  }
  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    execve(path.c_str(), argv.data(), envp);
    _exit(127);
  }
  result.started = true;
  close(in_pipe[0]);
  close(out_pipe[1]);
  close(err_pipe[1]);

  for (int fd : {in_pipe[1], out_pipe[0], err_pipe[0]}) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  }

  int stdin_fd = in_pipe[1];
  int out_fd = out_pipe[0];
  int err_fd = err_pipe[0];
  size_t written = 0;
  if (input.empty()) {
    close(stdin_fd);
    stdin_fd = -1;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool timed_out = false;
  while (stdin_fd >= 0 || out_fd >= 0 || err_fd >= 0) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd fds[3];
    int n = 0;
    int in_idx = -1, out_idx = -1, err_idx = -1;
    if (stdin_fd >= 0) {
      in_idx = n;
      fds[n++] = {stdin_fd, POLLOUT, 0};
    }
    if (out_fd >= 0) {
      out_idx = n;
      fds[n++] = {out_fd, POLLIN, 0};
    }
    if (err_fd >= 0) {
      err_idx = n;
      fds[n++] = {err_fd, POLLIN, 0};
    }
    if (poll(fds, n, static_cast<int>(left)) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (in_idx >= 0 && fds[in_idx].revents) {
      const ssize_t w = write(stdin_fd, input.data() + written, input.size() - written);
      if (w > 0)
        written += static_cast<size_t>(w);
      if (w < 0 && errno != EAGAIN && errno != EINTR)
        written = input.size();  // child gone
      if (written >= input.size()) {
        close(stdin_fd);
        stdin_fd = -1;
      }
    }
    for (auto [idx, fd, dest] :
         {std::tuple<int, int*, std::string*>{out_idx, &out_fd, &result.out},
          std::tuple<int, int*, std::string*>{err_idx, &err_fd, &result.err}}) {
      if (idx < 0 || !fds[idx].revents)
        continue;
      char buf[4096];
      const ssize_t r = read(*fd, buf, sizeof(buf));
      if (r > 0) {
        dest->append(buf, static_cast<size_t>(r));
      } else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
        close(*fd);
        *fd = -1;
      }
    }
  }

  for (int fd : {stdin_fd, out_fd, err_fd}) {
    if (fd >= 0)
      close(fd);
  }
  if (timed_out)
    kill(pid, SIGKILL);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!timed_out && WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  return result;
}

}  // namespace lhc
