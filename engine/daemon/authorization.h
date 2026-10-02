#pragma once

#include <sys/types.h>

#include <functional>
#include <optional>
#include <string>

#include "daemon/protocol.h"

namespace lhc {

// Maps a login name to its uid, if the user exists.
using UidLookup = std::function<std::optional<uid_t>(const std::string&)>;

// The uid of `name` from the passwd database.
std::optional<uid_t> systemUidLookup(const std::string& name);

// Who may send what (DESIGN.md): `root_uid` may send anything; anyone may ask for `status`; auth
// and list are also open to the user the request is about (the KDE lock screen runs PAM as that
// user); everything else is for root only.
bool isAuthorized(const Request& request, uid_t peer_uid, uid_t root_uid, const UidLookup& lookup);

}  // namespace lhc
