#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace lhc {

// Requests of the daemon protocol (DESIGN.md): one JSON object per line.
enum class Cmd { kAuth, kTest, kEnroll, kList, kRemove, kClear, kMigrate, kSetEncryption, kStatus };

struct Request {
  Cmd cmd = Cmd::kStatus;
  std::string username;
  std::string service;  // auth
  std::string id;       // remove
  std::string value;    // set-encryption: on | off | auto
  bool remote = false;  // auth: the PAM caller reported a remote session
  int count = 5;        // enroll
};

// Longest request line the daemon reads.
inline constexpr size_t kMaxRequestBytes = 4096;

inline constexpr int kMaxEnrollCount = 20;

// A login name that is safe to use in a file name: 1 to 64 of [A-Za-z0-9._@-], not starting with
// '.' or '-'.
bool validUsername(const std::string& name);

// Parses and validates one request line. False with `error` for anything malformed, unknown, or
// missing a field its command needs.
bool parseRequest(const std::string& line, Request& out, std::string& error);

// The request as the client sends it (without the trailing newline).
std::string requestToJson(const Request& request);

nlohmann::json errorReply(const std::string& detail);

// A reply on the wire: compact JSON and a newline.
std::string toLine(const nlohmann::json& reply);

}  // namespace lhc
