#include "daemon/engine.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>

#include "analysis/authenticator.h"
#include "analysis/backlight.h"
#include "analysis/crop_selector.h"
#include "analysis/frame_analyzer.h"
#include "camera/camera_stream.h"
#include "camera/frame_grabber.h"
#include "core/config.h"
#include "core/hardware.h"
#include "store/migrate.h"

namespace lhc {

namespace {

using Clock = std::chrono::steady_clock;
using nlohmann::json;

// Enrolling gives up this long after the camera session starts.
constexpr int kEnrollTimeoutMs = 20000;

// Once one camera's pictures are taken, enrolling waits at most this much longer for the other's.
constexpr int kColourEnrollGraceMs = 4000;

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

CameraStream cameraStream(const CameraOpener& opener, const std::string& device,
                          bool colour = false) {
  return CameraStream(
      [opener, device, colour](std::string& error) { return opener(device, colour, error); });
}

json reportReply(const AuthReport& r, const char* result) {
  json reply = {{"result", result},
                {"best_score", r.best_score},
                {"liveness_pairs", r.liveness_pairs},
                {"elapsed_ms", r.elapsed_ms},
                {"face_gain_median", r.face_gain_median},
                {"ratio_median", r.ratio_median}};
  if (r.used_colour) {
    reply["backlight"] = true;
  }
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
      case Cmd::kRename:
        return rename(request);
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
      (config.ai_antispoof.enabled || config.backlight.enabled
           ? config.ai_antispoof.model + "|" + std::to_string(config.ai_antispoof.threshold)
           : "") +
      "|" + (config.ai_antispoof.enabled ? "ir" : "") + (config.backlight.enabled ? "colour" : "");
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

  std::vector<std::vector<float>> enrolled, colour_enrolled;
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
      (e.kind == kColourKind ? colour_enrolled : enrolled).push_back(e.embedding);
    }
  }
  if (enrolled.empty()) {
    return refuse("not_enrolled", "template has no IR pictures");
  }
  const bool backlight = config.backlight.enabled && !config.backlight.camera.empty() &&
                         !colour_enrolled.empty();

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

  AuthReport report = runAuth(config, camera, models_, enrolled, start,
                              start + std::chrono::milliseconds(config.timeout_ms), backlight);
  if (report.backlit) {
    // Too dark for the IR checks: confirm with the colour camera too, with a fresh timeout.
    CameraStream colour = cameraStream(open_camera_, config.backlight.camera, true);
    if (colour.open(error)) {
      report = runBacklightAuth(config, camera, colour, models_, colour_enrolled, start,
                                Clock::now() + std::chrono::milliseconds(config.timeout_ms));
      colour.close();
    } else {
      spdlog::warn("backlight fallback skipped: colour camera: {}", error);
    }
  }
  camera.close();

  if (is_auth) {
    if (report.verdict == Verdict::kOk) {
      limiter_.recordSuccess(user);
    } else if (countsAsFailedAttempt(report.verdict)) {
      limiter_.recordFailure(user, Clock::now());
    }
  }
  spdlog::info("user={} service={} result={} best_score={:.3f} pairs={} elapsed_ms={}{}", user,
               service, verdictName(report.verdict), report.best_score, report.liveness_pairs,
               report.elapsed_ms, report.used_colour ? " backlight=1" : "");

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

  if (!request.face.empty() && !hasFace(user, request.face, config)) {
    return errorReply("no such face");
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

  // With the backlight fallback on, the colour camera takes the same number of pictures alongside.
  const bool with_colour = config.backlight.enabled && !config.backlight.camera.empty();
  CameraStream colour = cameraStream(open_camera_, config.backlight.camera, true);
  std::unique_ptr<FrameGrabber> grabber;
  if (with_colour) {
    if (!colour.open(error)) {
      return errorReply("colour camera unavailable: " + error);
    }
    grabber = std::make_unique<FrameGrabber>(colour);
  }
  CropSelectorParams colour_params;
  colour_params.require_liveness = false;  // the strobe is the IR camera's; nothing to pair here
  CropSelector colour_selector(colour_params);
  const int total = request.count * (with_colour ? 2 : 1);

  // Only embeddings are kept; the crop an observation carries never leaves memory.
  std::vector<TemplateEntry> added;
  int ir_added = 0, colour_added = 0;
  int64_t last_id = 0;
  auto add = [&](const std::vector<float>& embedding, const char* kind) {
    TemplateEntry entry;
    const int64_t now_ms = unixMs();
    last_id = std::max(now_ms, last_id + 1);  // two cameras can take pictures in the same ms
    entry.id = std::to_string(last_id);
    entry.created = now_ms / 1000;
    entry.embedding = embedding;
    entry.kind = kind;
    added.push_back(std::move(entry));
    return emit(json{{"progress", added.size()}, {"total", total}});
  };

  auto deadline = start + std::chrono::milliseconds(kEnrollTimeoutMs);
  bool grace = false;
  std::vector<Observation> observations;
  while (ir_added < request.count || (with_colour && colour_added < request.count)) {
    // Once one camera has its pictures, the other gets a little longer (only IR for a new face).
    const bool one_done = ir_added >= request.count ||
                          (!request.face.empty() && with_colour && colour_added >= request.count);
    if (one_done && !grace) {
      grace = true;
      deadline = std::min(deadline, Clock::now() + std::chrono::milliseconds(kColourEnrollGraceMs));
    }
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
      if (ir_added >= request.count || !selector.accept(obs)) {
        continue;
      }
      ++ir_added;
      if (!add(obs.embedding, kIrKind)) {
        return errorReply("client went away");
      }
    }

    Frame colour_frame;
    if (grabber && colour_added < request.count && grabber->take(colour_frame) &&
        !colour_frame.colour.empty()) {
      const std::vector<Detection> faces = models_.detector->inference(colour_frame.colour);
      if (!faces.empty()) {
        Observation obs;
        obs.timestamp_us = colour_frame.timestamp_us;
        obs.face_found = true;
        obs.det_score = faces.front().conf;
        obs.embedding = models_.recognizer->embed(faces.front().image);
        if (colour_selector.accept(obs)) {
          ++colour_added;
          if (!add(obs.embedding, kColourKind)) {
            return errorReply("client went away");
          }
        }
      }
    }
  }
  grabber.reset();
  colour.close();
  camera.close();

  // More pictures of an existing face may be colour only: in front of a bright window the IR
  // pictures cannot pass the strobe check, and that is exactly when colour pictures are wanted.
  const bool colour_only = !request.face.empty() && with_colour && colour_added >= request.count;
  if (ir_added < request.count && !colour_only) {
    return errorReply("timeout");
  }
  if (colour_only && ir_added < request.count) {
    added.erase(std::remove_if(added.begin(), added.end(),
                               [](const TemplateEntry& e) { return e.kind == kIrKind; }),
                added.end());
    ir_added = 0;
  }
  if (with_colour && colour_added < request.count) {
    spdlog::warn("user={} only {} of {} colour pictures taken", user, colour_added, request.count);
  }

  std::lock_guard<std::mutex> lock(store_mutex_);
  const Storage target = effectiveStorage(config);
  reconcileUser(user, target);
  LoadResult existing = store_.load(user, config.recognition.model, target);
  Template tmpl;
  if (existing.status == LoadStatus::kOk) {
    tmpl = std::move(existing.tmpl);
  } else if (!request.face.empty()) {
    return errorReply("no such face");
  } else {
    // Nothing yet, or a template the configured model can't use (or that can't be decrypted):
    // start again; this is how the user recovers from "needs re-enrolment".
    tmpl.model = config.recognition.model;
    tmpl.dim = static_cast<int>(added.front().embedding.size());
  }
  // A new face is named after its first picture.
  const std::string face = request.face.empty() ? added.front().id : request.face;
  if (!request.face.empty() &&
      std::none_of(tmpl.entries.begin(), tmpl.entries.end(),
                   [&](const TemplateEntry& e) { return e.face == face; })) {
    return errorReply("no such face");  // removed while the pictures were taken
  }
  for (TemplateEntry& e : added) {
    e.face = face;
    tmpl.entries.push_back(std::move(e));
  }
  if (!request.name.empty()) {
    tmpl.names[face] = request.name;
  }
  if (!store_.save(user, tmpl, target, error)) {
    return errorReply("cannot save template: " + error);
  }
  spdlog::info("user={} enrolled {} IR and {} colour picture(s) for face {}, template has {}", user,
               ir_added, colour_added, face, tmpl.entries.size());
  return json{{"result", "ok"}, {"added", ir_added}, {"colour_added", colour_added}, {"face", face}};
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
    entries.push_back({{"id", e.id}, {"created", e.created}, {"face", e.face}, {"kind", e.kind}});
  }
  return json{{"result", "ok"},
              {"entries", entries},
              {"names", loaded.tmpl.names},
              {"encryption", storageName(loaded.storage)},
              {"model", loaded.tmpl.model}};
}

json Engine::remove(const Request& request) {
  const Config config = configOrDefault();
  std::lock_guard<std::mutex> lock(store_mutex_);
  bool found = false;
  std::string error;
  if (!store_.removeEntry(request.username, request.id, config.recognition.model,
                          effectiveStorage(config), found, error, request.face)) {
    return errorReply(error);
  }
  return found ? json{{"result", "ok"}}
               : errorReply(request.face.empty() ? "no such entry" : "no such face");
}

json Engine::rename(const Request& request) {
  const Config config = configOrDefault();
  const Storage target = effectiveStorage(config);
  std::lock_guard<std::mutex> lock(store_mutex_);
  reconcileUser(request.username, target);
  LoadResult loaded = store_.load(request.username, config.recognition.model, target);
  if (loaded.status != LoadStatus::kOk ||
      std::none_of(loaded.tmpl.entries.begin(), loaded.tmpl.entries.end(),
                   [&](const TemplateEntry& e) { return e.face == request.face; })) {
    return errorReply("no such face");
  }
  loaded.tmpl.names[request.face] = request.name;
  std::string error;
  if (!store_.save(request.username, loaded.tmpl, target, error)) {
    return errorReply("cannot save template: " + error);
  }
  return json{{"result", "ok"}};
}

bool Engine::hasFace(const std::string& user, const std::string& face, const Config& config) {
  std::lock_guard<std::mutex> lock(store_mutex_);
  const LoadResult loaded = store_.load(user, config.recognition.model, effectiveStorage(config));
  return loaded.status == LoadStatus::kOk &&
         std::any_of(loaded.tmpl.entries.begin(), loaded.tmpl.entries.end(),
                     [&](const TemplateEntry& e) { return e.face == face; });
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
