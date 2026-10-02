#include "daemon/engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>

#include "analysis/authenticator.h"
#include "analysis/crop_selector.h"
#include "analysis/frame_analyzer.h"
#include "camera/camera_stream.h"
#include "core/config.h"
#include "core/hardware.h"
#include "store/migrate.h"

namespace lhc {

namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

// Enrolling gives up this long after the camera session starts.
constexpr int kEnrollTimeoutMs = 20000;

int64_t unixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Only a face that was there and did not match counts against the user: nobody in front of the
// camera is not an attack.
bool countsAsFailedAttempt(Verdict verdict) {
  return verdict == Verdict::kNotRecognised || verdict == Verdict::kLivenessFailed;
}

CameraStream cameraStream(const CameraOpener& opener, const std::string& device) {
  return CameraStream([opener, device](std::string& error) { return opener(device, error); });
}

json reportReply(const AuthReport& r, const char* result) {
  json reply = {{"result", result},
                {"best_score", r.best_score},
                {"liveness_pairs", r.liveness_pairs},
                {"elapsed_ms", r.elapsed_ms},
                {"face_gain_median", r.face_gain_median},
                {"ratio_median", r.ratio_median}};
  if (!r.detail.empty()) {
    reply["detail"] = r.detail;
  }
  return reply;
}

}  // namespace

json Engine::handle(const Request& request, const Emit& emit) {
  try {
    switch (request.cmd) {
      case Cmd::kAuth:
        return verify(request, true);
      case Cmd::kTest:
        return verify(request, false);
      case Cmd::kEnroll:
        return enroll(request, emit);
      case Cmd::kList:
        return list(request);
      case Cmd::kRemove:
        return remove(request);
      case Cmd::kClear:
        return clear(request);
      case Cmd::kMigrate:
        return migrate();
      case Cmd::kSetEncryption:
        return setEncryption(request);
      case Cmd::kStatus:
        return status();
    }
  } catch (const std::exception& e) {
    spdlog::error("request failed: {}", e.what());
  }
  return errorReply("internal error");
}

bool Engine::hasRecentFailures() { return limiter_.hasRecentFailures(Clock::now()); }

Config Engine::configOrDefault() const {
  Config config;
  std::string error;
  if (!loadConfig(options_.config_path, config, error)) {
    return Config();
  }
  return config;
}

Storage Engine::effectiveStorage(const Config& config) const {
  return chooseStorage(config.storage.tpm_encryption, detectHardware(options_.hardware_root));
}

void Engine::reconcileUser(const std::string& user, Storage target) {
  std::string error;
  switch (store_.convert(user, target, error)) {
    case ConvertStatus::kConverted:
      spdlog::info("template of {} converted to {}", user, storageName(target));
      break;
    case ConvertStatus::kFailed:
      spdlog::warn("template of {} not converted to {}: {}", user, storageName(target), error);
      break;
    case ConvertStatus::kUnchanged:
      break;
  }
}

void Engine::reconcileAll() {
  const Storage target = effectiveStorage(configOrDefault());
  std::lock_guard<std::mutex> lock(store_mutex_);
  for (const std::string& user : store_.users()) {
    reconcileUser(user, target);
  }
}

bool Engine::ensureModels(const Config& config, std::string& error) {
  const std::string key =
      config.detection.model + "|" + std::to_string(config.detection.threshold) + "|" +
      config.recognition.model + "|" +
      (config.ai_antispoof.enabled
           ? config.ai_antispoof.model + "|" + std::to_string(config.ai_antispoof.threshold)
           : "");
  if (models_.detector && key == models_key_) {
    return true;
  }
  models_ = Models();
  if (!loadModels(config, options_.models_dir, models_, error)) {
    models_ = Models();
    return false;
  }
  models_key_ = key;
  return true;
}

json Engine::verify(const Request& request, bool is_auth) {
  const std::string& user = request.username;
  const std::string service = request.service.empty() ? "-" : request.service;

  // `result` is what `test` reports; `auth` turns every case that is not about the face into
  // "ignore" so PAM falls through to the password.
  auto refuse = [&](const char* test_result, const std::string& detail) {
    spdlog::info("user={} service={} {}: {}", user, service, is_auth ? "ignored" : test_result,
                 detail);
    return json{{"result", is_auth ? "ignore" : test_result}, {"detail", detail}};
  };

  Config config;
  std::string error;
  if (!loadConfig(options_.config_path, config, error)) {
    return refuse("error", "no usable config");
  }
  if (is_auth) {
    if (config.disabled) {
      return refuse("ignore", "disabled");
    }
    if (std::find(config.ignore_services.begin(), config.ignore_services.end(), request.service) !=
        config.ignore_services.end()) {
      return refuse("ignore", "service ignored");
    }
    if (request.remote && config.abort_if_ssh) {
      return refuse("ignore", "remote session");
    }
    if (limiter_.blocked(user, Clock::now())) {
      return refuse("ignore", "too many failed attempts");
    }
  }
  if (config.camera.empty()) {
    return refuse("camera_unavailable", "no camera configured");
  }

  std::vector<std::vector<float>> enrolled;
  {
    std::lock_guard<std::mutex> lock(store_mutex_);
    const Storage target = effectiveStorage(config);
    reconcileUser(user, target);
    const LoadResult loaded = store_.load(user, config.recognition.model, target);
    if (loaded.status == LoadStatus::kNotEnrolled) {
      return refuse("not_enrolled", "no template");
    }
    if (loaded.status == LoadStatus::kNeedsReenrol) {
      return refuse("needs_reenrol", "needs re-enrolment: " + loaded.detail);
    }
    for (const TemplateEntry& e : loaded.tmpl.entries) {
      enrolled.push_back(e.embedding);
    }
  }
  if (enrolled.empty()) {
    return refuse("not_enrolled", "template has no entries");
  }

  SessionGate::Lock session = gate_.acquire(options_.busy_wait);
  if (!session) {
    spdlog::info("user={} service={} busy", user, service);
    return json{{"result", "busy"}};
  }
  const auto start = Clock::now();

  CameraStream camera = cameraStream(open_camera_, config.camera);
  if (!camera.open(error)) {
    return refuse("camera_unavailable", error);
  }
  if (!ensureModels(config, error)) {
    return refuse("error", error);
  }

  const AuthReport report = runAuth(config, camera, models_, enrolled, start,
                                    start + std::chrono::milliseconds(config.timeout_ms));
  camera.close();

  if (is_auth) {
    if (report.verdict == Verdict::kOk) {
      limiter_.recordSuccess(user);
    } else if (countsAsFailedAttempt(report.verdict)) {
      limiter_.recordFailure(user, Clock::now());
    }
  }
  spdlog::info("user={} service={} result={} best_score={:.3f} pairs={} elapsed_ms={}", user,
               service, verdictName(report.verdict), report.best_score, report.liveness_pairs,
               report.elapsed_ms);

  const bool not_face_related =
      report.verdict == Verdict::kCameraUnavailable || report.verdict == Verdict::kError;
  return reportReply(report, is_auth && not_face_related ? "ignore" : verdictName(report.verdict));
}

json Engine::enroll(const Request& request, const Emit& emit) {
  const std::string& user = request.username;
  Config config;
  std::string error;
  if (!loadConfig(options_.config_path, config, error)) {
    return errorReply("no usable config");
  }
  if (config.camera.empty()) {
    return errorReply("no camera configured");
  }

  SessionGate::Lock session = gate_.acquire(options_.busy_wait);
  if (!session) {
    return json{{"result", "busy"}};
  }
  const auto start = Clock::now();

  CameraStream camera = cameraStream(open_camera_, config.camera);
  if (!camera.open(error)) {
    return errorReply("camera unavailable: " + error);
  }
  if (!ensureModels(config, error)) {
    return errorReply(error);
  }

  CropSelectorParams params;
  params.require_liveness = config.ir_liveness.enabled;
  params.gain = {config.ir_liveness.min_face_gain, config.ir_liveness.min_gain_ratio};
  CropSelector selector(params);
  FrameAnalyzer analyzer(*models_.detector, models_.recognizer.get());

  // Only embeddings are kept; the crop an observation carries never leaves memory.
  std::vector<TemplateEntry> added;
  const auto deadline = start + std::chrono::milliseconds(kEnrollTimeoutMs);
  std::vector<Observation> observations;
  while (static_cast<int>(added.size()) < request.count) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) {
      break;
    }
    Frame frame;
    const ReadStatus status = camera.next(frame, static_cast<int>(left));
    if (status == ReadStatus::kError) {
      break;
    }
    if (status == ReadStatus::kTimeout) {
      continue;
    }
    observations.clear();
    analyzer.push(std::move(frame), observations);
    for (const Observation& obs : observations) {
      if (static_cast<int>(added.size()) >= request.count || !selector.accept(obs)) {
        continue;
      }
      TemplateEntry entry;
      const int64_t now_ms = unixMs();
      entry.id = std::to_string(now_ms);
      entry.created = now_ms / 1000;
      entry.embedding = obs.embedding;
      added.push_back(std::move(entry));
      if (!emit(json{{"progress", added.size()}, {"total", request.count}})) {
        return errorReply("client went away");
      }
    }
  }
  camera.close();

  if (static_cast<int>(added.size()) < request.count) {
    return errorReply("timeout");
  }

  std::lock_guard<std::mutex> lock(store_mutex_);
  const Storage target = effectiveStorage(config);
  reconcileUser(user, target);
  LoadResult existing = store_.load(user, config.recognition.model, target);
  Template tmpl;
  if (existing.status == LoadStatus::kOk) {
    tmpl = std::move(existing.tmpl);
  } else {
    // Nothing yet, or a template the configured model can't use (or that can't be decrypted):
    // start again; this is how the user recovers from "needs re-enrolment".
    tmpl.model = config.recognition.model;
    tmpl.dim = static_cast<int>(added.front().embedding.size());
  }
  for (TemplateEntry& e : added) {
    tmpl.entries.push_back(std::move(e));
  }
  if (!store_.save(user, tmpl, target, error)) {
    return errorReply("cannot save template: " + error);
  }
  spdlog::info("user={} enrolled {} face(s), template has {}", user, request.count,
               tmpl.entries.size());
  return json{{"result", "ok"}, {"added", request.count}};
}

json Engine::list(const Request& request) {
  const Config config = configOrDefault();
  const Storage target = effectiveStorage(config);
  std::lock_guard<std::mutex> lock(store_mutex_);
  reconcileUser(request.username, target);
  const LoadResult loaded = store_.load(request.username, config.recognition.model, target);
  switch (loaded.status) {
    case LoadStatus::kNotEnrolled:
      return json{{"result", "not_enrolled"}};
    case LoadStatus::kNeedsReenrol:
      return json{{"result", "needs_reenrol"}, {"detail", loaded.detail}};
    case LoadStatus::kOk:
      break;
  }
  json entries = json::array();
  for (const TemplateEntry& e : loaded.tmpl.entries) {
    entries.push_back({{"id", e.id}, {"created", e.created}});
  }
  return json{{"result", "ok"},
              {"entries", entries},
              {"encryption", storageName(loaded.storage)},
              {"model", loaded.tmpl.model}};
}

json Engine::remove(const Request& request) {
  const Config config = configOrDefault();
  std::lock_guard<std::mutex> lock(store_mutex_);
  bool found = false;
  std::string error;
  if (!store_.removeEntry(request.username, request.id, config.recognition.model,
                          effectiveStorage(config), found, error)) {
    return errorReply(error);
  }
  return found ? json{{"result", "ok"}} : errorReply("no such entry");
}

json Engine::clear(const Request& request) {
  std::lock_guard<std::mutex> lock(store_mutex_);
  std::string error;
  if (!store_.clear(request.username, error)) {
    return errorReply(error);
  }
  spdlog::info("user={} template removed", request.username);
  return json{{"result", "ok"}};
}

json Engine::migrate() {
  const Config config = configOrDefault();
  SessionGate::Lock session = gate_.acquire(options_.busy_wait);
  if (!session) {
    return json{{"result", "busy"}};
  }
  std::string error;
  if (!ensureModels(config, error)) {
    return errorReply(error);
  }

  std::lock_guard<std::mutex> lock(store_mutex_);
  const MigrateResult result =
      migrateFaces(options_.faces_dir, store_, config.recognition.model, effectiveStorage(config),
                   [this](const ImageRGB& crop) { return models_.recognizer->embed(crop); });
  json failed = json::array();
  for (const auto& [user, why] : result.failed) {
    failed.push_back({{"username", user}, {"detail", why}});
    spdlog::warn("migrate: {} kept its crops: {}", user, why);
  }
  for (const std::string& user : result.migrated) {
    spdlog::info("migrate: {} converted to a template", user);
  }
  return json{{"result", "ok"}, {"migrated", result.migrated}, {"failed", failed}};
}

json Engine::setEncryption(const Request& request) {
  TpmSetting setting;
  parseSetting(request.value, setting);  // validated by the parser
  const Hardware hardware = detectHardware(options_.hardware_root);
  if (setting == TpmSetting::kOn && !hardware.tpm2) {
    return errorReply("no TPM 2.0");
  }

  std::string error;
  if (!writeTpmSetting(options_.config_path, setting, error)) {
    return errorReply(error);
  }
  const Storage target = chooseStorage(setting, hardware);

  std::lock_guard<std::mutex> lock(store_mutex_);
  int converted = 0;
  json failed = json::array();
  for (const std::string& user : store_.users()) {
    std::string why;
    switch (store_.convert(user, target, why)) {
      case ConvertStatus::kConverted:
        converted++;
        break;
      case ConvertStatus::kFailed:
        failed.push_back({{"username", user}, {"detail", why}});
        break;
      case ConvertStatus::kUnchanged:
        break;
    }
  }
  spdlog::info("tpm_encryption={} effective={} converted={} failed={}", settingName(setting),
               storageName(target), converted, failed.size());
  json reply = {{"result", "ok"}, {"effective", storageName(target)}, {"converted", converted}};
  if (!failed.empty()) {
    reply["failed"] = failed;
  }
  return reply;
}

json Engine::status() {
  const Config config = configOrDefault();
  const Hardware hardware = detectHardware(options_.hardware_root);
  return json{{"tpm2", hardware.tpm2},
              {"secure_boot", hardware.secure_boot},
              {"setting", settingName(config.storage.tpm_encryption)},
              {"effective", storageName(chooseStorage(config.storage.tpm_encryption, hardware))}};
}

}  // namespace lhc
