#include "cli/confirm_hook.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#include "core/config.h"

namespace lhc {

bool runConfirmHook(const std::string& hook_path, const std::string& username,
                    const std::string& service) {
  // Built before fork(): the child may only call async-signal-safe functions.
  const std::string pam_pid = std::to_string(getppid());

  const pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    const int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
    }
    execl(hook_path.c_str(), "confirm-hook", "--user", username.c_str(), "--service",
          service.c_str(), "--pam-pid", pam_pid.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace lhc
