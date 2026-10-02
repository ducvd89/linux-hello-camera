#pragma once

#include <string>
#include <vector>

namespace lhc {

struct ProcessResult {
  bool started = false;  // false if fork/exec itself failed
  int exit_code = -1;    // -1 if killed by a signal or timed out
  std::string out;
  std::string err;
};

// Runs `path` with `args` (argv[0] is added), an empty environment and no shell. `input` goes to
// stdin through a pipe and stdout/stderr come back through pipes, so secrets never touch disk.
// Kills the child after `timeout_ms`.
ProcessResult runProcess(const std::string& path, const std::vector<std::string>& args,
                         const std::string& input, int timeout_ms);

}  // namespace lhc
