#pragma once

#include <string>

#include "cli/daemon_client.h"

namespace lhc {

// Exit codes of `auth`, as the PAM module maps them.
inline constexpr int kExitMatch = 0;
inline constexpr int kExitNoMatch = 1;
inline constexpr int kExitIgnore = 2;

// What PAM runs. Does the checks that need the caller's context (config switches, remote session,
// lid), asks the daemon for the face check, then runs the confirm hook. Never writes to stdout or
// stderr (the caller must have redirected them); logs to syslog. `remote` is set by the PAM module
// when PAM_RHOST is non-empty. Returns kExitMatch, kExitNoMatch or kExitIgnore; a daemon that
// cannot be reached or is busy is "ignore".
int runAuthCommand(const ClientContext& context, const std::string& username,
                   const std::string& service, bool remote);

}  // namespace lhc
