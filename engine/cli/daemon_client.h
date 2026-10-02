#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

#include "core/config.h"
#include "daemon/protocol.h"

namespace lhc {

// Where the helper finds the daemon, the system config and the confirmation hook. The defaults are
// the installed locations; tests point them elsewhere.
struct ClientContext {
  std::string socket_path = kSocketPath;
  std::string config_path = kDefaultConfigPath;
  std::string confirm_hook = kConfirmHook;
};

enum class CallStatus {
  kOk,           // `reply` holds the daemon's final answer
  kUnreachable,  // could not connect (daemon not installed, socket not active)
  kBroken,       // connected, but no usable answer (timeout, closed early, garbage)
};

struct CallResult {
  CallStatus status = CallStatus::kUnreachable;
  nlohmann::json reply;
  std::string error;
};

// How long to wait for the socket to accept a connection.
inline constexpr int kConnectTimeoutMs = 3000;

// Sends `request` and collects the answer. `{"progress":..}` lines go to `on_progress`; the first
// other line is the reply. `read_timeout_ms` bounds the wait for it (socket activation has to start
// the daemon and load the models first).
CallResult callDaemon(const ClientContext& context, const Request& request, int read_timeout_ms,
                      const std::function<void(const nlohmann::json&)>& on_progress = nullptr);

}  // namespace lhc
