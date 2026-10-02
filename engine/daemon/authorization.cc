#include "daemon/authorization.h"

#include <pwd.h>

namespace lhc {

std::optional<uid_t> systemUidLookup(const std::string& name) {
  const passwd* pw = getpwnam(name.c_str());
  if (pw == nullptr) {
    return std::nullopt;
  }
  return pw->pw_uid;
}

bool isAuthorized(const Request& request, uid_t peer_uid, uid_t root_uid, const UidLookup& lookup) {
  if (peer_uid == root_uid) {
    return true;
  }
  switch (request.cmd) {
    case Cmd::kStatus:
      return true;
    case Cmd::kAuth:
    case Cmd::kList: {
      const std::optional<uid_t> uid = lookup(request.username);
      return uid.has_value() && *uid == peer_uid;
    }
    default:
      return false;
  }
}

}  // namespace lhc
