#include "core/file_util.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace lhc {

namespace {

std::string errnoText(const std::string& what) { return what + ": " + std::strerror(errno); }

}  // namespace

bool atomicWriteFile(const std::string& path, const std::string& content, unsigned mode,
                     std::string& error) {
  const size_t slash = path.rfind('/');
  const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
  const std::string tmp = dir + "/.tmp." + std::to_string(getpid()) + "." +
                          path.substr(slash == std::string::npos ? 0 : slash + 1);

  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    error = errnoText("create " + tmp);
    return false;
  }
  size_t done = 0;
  bool ok = true;
  while (ok && done < content.size()) {
    const ssize_t n = write(fd, content.data() + done, content.size() - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      error = errnoText("write " + tmp);
      ok = false;
    } else {
      done += static_cast<size_t>(n);
    }
  }
  if (ok && (fchmod(fd, mode) != 0 || fsync(fd) != 0)) {
    error = errnoText("fsync " + tmp);
    ok = false;
  }
  close(fd);
  if (ok && rename(tmp.c_str(), path.c_str()) != 0) {
    error = errnoText("rename to " + path);
    ok = false;
  }
  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }
  const int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
  }
  return true;
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::ostringstream text;
  text << file.rdbuf();
  out = text.str();
  return true;
}

}  // namespace lhc
