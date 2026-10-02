#pragma once

#include <string>

#include "cli/daemon_client.h"

namespace lhc {

// The helper commands that are thin clients of the daemon. Each prints the daemon's answer in the
// format DESIGN.md gives and returns the exit code.

// `test`: the daemon's reply as one JSON line; 0 if the result is "ok".
int runTestCommand(const ClientContext& context, const std::string& username);

// `enroll`: `PROGRESS i N` lines, then `OK n` or `ERR <reason>`.
int runEnrollCommand(const ClientContext& context, const std::string& username, int count);

// `list` and `status`: the reply as one JSON line; 0 unless the daemon could not be asked.
int runQueryCommand(const ClientContext& context, const Request& request);

// `remove`, `clear`, `migrate`, `set-encryption`: the reply as one JSON line; 0 if it says "ok".
int runChangeCommand(const ClientContext& context, const Request& request);

}  // namespace lhc
