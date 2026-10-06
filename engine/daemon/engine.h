#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "analysis/models.h"
#include "camera/frame_source.h"
#include "daemon/protocol.h"
#include "daemon/rate_limiter.h"
#include "daemon/session_gate.h"
#include "store/template_store.h"

namespace lhc {

// Opens the camera named in the config (a device path), or returns null with `error` set. Tests
// plug in a replay of recorded frames.
using CameraOpener =
    std::function<std::unique_ptr<FrameSource>(const std::string& device, std::string& error)>;

// Sends one reply line to the client; false once the client has gone away.
using Emit = std::function<bool(const nlohmann::json&)>;

struct EngineOptions {
  std::string config_path;
  std::string models_dir;
  std::string faces_dir;      // the 0.8 crops, read by migrate only
  std::string hardware_root;  // prefix for /dev and /sys when detecting the TPM; "" in production
  std::chrono::milliseconds busy_wait{5000};
};

// What the daemon does for each request, independent of sockets. Authorisation is the server's
// business; by the time a request gets here it is allowed. Thread safe: camera sessions and the
// loaded models are used one at a time (SessionGate), template files are serialised by a mutex.
class Engine {
 public:
  Engine(EngineOptions options, TemplateStore& store, CameraOpener open_camera)
      : options_(std::move(options)), store_(store), open_camera_(std::move(open_camera)) {}

  // Handles a request, emitting progress lines through `emit`, and returns the final reply.
  nlohmann::json handle(const Request& request, const Emit& emit);

  // Converts every user's template to the effective storage mode (on startup).
  void reconcileAll();

  bool hasRecentFailures();

 private:
  nlohmann::json verify(const Request& request, bool is_auth);
  nlohmann::json enroll(const Request& request, const Emit& emit);
  nlohmann::json list(const Request& request);
  nlohmann::json remove(const Request& request);
  nlohmann::json rename(const Request& request);
  nlohmann::json clear(const Request& request);
  // Whether the user's readable template has pictures of `face`.
  bool hasFace(const std::string& user, const std::string& face, const Config& config);
  nlohmann::json migrate();
  nlohmann::json setEncryption(const Request& request);
  nlohmann::json status();

  Config configOrDefault() const;
  Storage effectiveStorage(const Config& config) const;
  void reconcileUser(const std::string& user, Storage target);  // store_mutex_ held
  bool ensureModels(const Config& config, std::string& error);

  EngineOptions options_;
  TemplateStore& store_;
  CameraOpener open_camera_;
  RateLimiter limiter_;
  SessionGate gate_;
  std::mutex store_mutex_;

  // Used only while holding the gate.
  Models models_;
  std::string models_key_;
};

}  // namespace lhc
