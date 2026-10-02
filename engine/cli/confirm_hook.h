#pragma once

#include <string>

namespace lhc {

// Runs the confirm hook (/usr/lib/linux-hello-camera/confirm-hook) for a matched face and returns
// true only if it exits 0. The hook asks the user to approve sudo/admin prompts and enforces its
// own timeout, so this waits without one. Its stdio is /dev/null (see the note in pam/helper.cc).
bool runConfirmHook(const std::string& hook_path, const std::string& username,
                    const std::string& service);

}  // namespace lhc
