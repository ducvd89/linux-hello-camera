#include "daemon/protocol.h"

#include <algorithm>

namespace lhc {

namespace {

struct CmdName {
  Cmd cmd;
  const char* name;
};

constexpr CmdName kCmds[] = {
    {Cmd::kAuth, "auth"},       {Cmd::kTest, "test"},
    {Cmd::kEnroll, "enroll"},   {Cmd::kList, "list"},
    {Cmd::kRemove, "remove"},   {Cmd::kClear, "clear"},
    {Cmd::kMigrate, "migrate"}, {Cmd::kSetEncryption, "set-encryption"},
    {Cmd::kStatus, "status"},
};

bool needsUsername(Cmd cmd) {
  return cmd != Cmd::kMigrate && cmd != Cmd::kSetEncryption && cmd != Cmd::kStatus;
}

// Reads an optional string field; false if it is present but not a string.
bool optionalString(const nlohmann::json& doc, const char* key, std::string& out) {
  const auto it = doc.find(key);
  if (it == doc.end() || it->is_null()) {
    return true;
  }
  if (!it->is_string()) {
    return false;
  }
  out = it->get<std::string>();
  return true;
}

}  // namespace

bool validUsername(const std::string& name) {
  if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '-') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '_' || c == '@' || c == '-';
  });
}

bool parseRequest(const std::string& line, Request& out, std::string& error) {
  if (line.size() > kMaxRequestBytes) {
    error = "request too long";
    return false;
  }
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(line);
  } catch (const nlohmann::json::exception&) {
    error = "malformed request";
    return false;
  }
  if (!doc.is_object() || !doc.contains("cmd") || !doc["cmd"].is_string()) {
    error = "malformed request";
    return false;
  }

  Request r;
  const std::string name = doc["cmd"].get<std::string>();
  const auto cmd = std::find_if(std::begin(kCmds), std::end(kCmds),
                                [&](const CmdName& c) { return name == c.name; });
  if (cmd == std::end(kCmds)) {
    error = "unknown command";
    return false;
  }
  r.cmd = cmd->cmd;

  if (!optionalString(doc, "username", r.username) || !optionalString(doc, "service", r.service) ||
      !optionalString(doc, "id", r.id) || !optionalString(doc, "value", r.value)) {
    error = "malformed request";
    return false;
  }
  if (doc.contains("remote")) {
    if (!doc["remote"].is_boolean()) {
      error = "malformed request";
      return false;
    }
    r.remote = doc["remote"].get<bool>();
  }
  if (doc.contains("count")) {
    if (!doc["count"].is_number_integer()) {
      error = "malformed request";
      return false;
    }
    r.count = doc["count"].get<int>();
  }

  if (needsUsername(r.cmd) && !validUsername(r.username)) {
    error = "bad username";
    return false;
  }
  if (r.cmd == Cmd::kEnroll && (r.count < 1 || r.count > kMaxEnrollCount)) {
    error = "bad count";
    return false;
  }
  if (r.cmd == Cmd::kRemove && r.id.empty()) {
    error = "missing id";
    return false;
  }
  if (r.cmd == Cmd::kSetEncryption && r.value != "on" && r.value != "off" && r.value != "auto") {
    error = "bad value";
    return false;
  }
  out = std::move(r);
  return true;
}

std::string requestToJson(const Request& request) {
  nlohmann::json doc;
  for (const CmdName& c : kCmds) {
    if (c.cmd == request.cmd) {
      doc["cmd"] = c.name;
    }
  }
  if (needsUsername(request.cmd)) {
    doc["username"] = request.username;
  }
  switch (request.cmd) {
    case Cmd::kAuth:
      doc["service"] = request.service;
      doc["remote"] = request.remote;
      break;
    case Cmd::kEnroll:
      doc["count"] = request.count;
      break;
    case Cmd::kRemove:
      doc["id"] = request.id;
      break;
    case Cmd::kSetEncryption:
      doc["value"] = request.value;
      break;
    default:
      break;
  }
  return doc.dump();
}

nlohmann::json errorReply(const std::string& detail) {
  return {{"result", "error"}, {"detail", detail}};
}

std::string toLine(const nlohmann::json& reply) { return reply.dump() + "\n"; }

}  // namespace lhc
