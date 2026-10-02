#include "cli/probe_command.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>

#include "analysis/crop_selector.h"
#include "analysis/frame_analyzer.h"
#include "analysis/models.h"
#include "camera/camera_stream.h"
#include "camera/replay_source.h"
#include "cli/output.h"
#include "core/config.h"
#include "face/enrolled_faces.h"

namespace lhc {

namespace {

// Everything probe and probe-dir share: config, models, and the enrolled embeddings to score
// against (none when no faces directory was given).
struct ProbeContext {
  Config config;
  Models models;
  std::vector<std::vector<float>> enrolled;
};

bool prepare(const ProbeOptions& options, ProbeContext& ctx, std::string& error) {
  const std::string config_path =
      options.config_path.empty() ? kDefaultConfigPath : options.config_path;
  if (!loadConfig(config_path, ctx.config, error)) {
    // Calibration must work before the app ever wrote a config.
    spdlog::warn("{}; using defaults", error);
    ctx.config = Config();
  }
  if (!loadModels(ctx.config, modelsDir(true), ctx.models, error)) {
    return false;
  }
  if (!options.faces_dir.empty()) {
    ctx.enrolled = embedEnrolled(*ctx.models.recognizer, options.faces_dir);
  }
  return true;
}

std::string describe(const Observation& obs, const ProbeContext& ctx) {
  if (!obs.face_found) {
    return fmt::format("{{\"lit_seq\": {}, \"lit_mean\": {:.1f}, \"face\": false}}", obs.lit_seq,
                       obs.lit_mean);
  }
  std::string line = fmt::format(
      "{{\"lit_seq\": {}, \"lit_mean\": {:.1f}, \"face\": true, \"det_score\": {:.3f}, "
      "\"box\": [{}, {}, {}, {}]",
      obs.lit_seq, obs.lit_mean, obs.det_score, obs.box.x1, obs.box.y1, obs.box.x2, obs.box.y2);
  if (obs.paired) {
    const GainParams params{ctx.config.ir_liveness.min_face_gain,
                            ctx.config.ir_liveness.min_gain_ratio};
    line += fmt::format(
        ", \"unlit_seq\": {}, \"dt_ms\": {:.1f}, \"face_gain\": {:.2f}, \"bg_gain\": {:.2f}, "
        "\"ratio\": {:.2f}, \"pass\": {}",
        obs.unlit_seq, obs.pair_gap_us / 1000.0, obs.gain.face_gain, obs.gain.bg_gain,
        obs.gain.ratio, gainPasses(obs.gain, params) ? "true" : "false");
  } else {
    line += ", \"unlit_seq\": null";
  }
  line += fmt::format(", \"specular\": {}", obs.specular);
  if (!ctx.enrolled.empty() && !obs.embedding.empty()) {
    float best = -1.0f;
    for (const auto& e : ctx.enrolled) {
      best = std::max(best, cosineSimilarity(obs.embedding, e));
    }
    line += fmt::format(", \"recognition\": {:.4f}", best);
  }
  return line + "}";
}

}  // namespace

int runProbe(const ProbeOptions& options) {
  ProbeContext ctx;
  std::string error;
  try {
    if (!prepare(options, ctx, error)) {
      spdlog::error("{}", error);
      return 1;
    }
    const std::string device = options.camera.empty() ? ctx.config.camera : options.camera;
    if (device.empty()) {
      spdlog::error("no camera: pass --camera or set camera in the config");
      return 1;
    }
    CameraStream camera(device);
    if (!camera.open(error)) {
      spdlog::error("{}", error);
      return 1;
    }

    FrameAnalyzer analyzer(*ctx.models.detector, ctx.models.recognizer.get());
    const auto start = std::chrono::steady_clock::now();
    const auto deadline =
        start + std::chrono::milliseconds(static_cast<int>(options.seconds * 1000));
    std::vector<Observation> observations;
    int frames = 0;
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) {
        break;
      }
      Frame frame;
      const ReadStatus status = camera.next(frame, static_cast<int>(left));
      if (status == ReadStatus::kError) {
        spdlog::error("camera read failed");
        return 1;
      }
      if (status == ReadStatus::kTimeout) {
        continue;
      }
      ++frames;
      observations.clear();
      analyzer.push(std::move(frame), observations);
      for (const Observation& obs : observations) {
        printLine(describe(obs, ctx));
      }
    }
    observations.clear();
    analyzer.flush(observations);
    for (const Observation& obs : observations) {
      printLine(describe(obs, ctx));
    }
    spdlog::info("probe: {} frames, {} reopens", frames, camera.reopenCount());
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }
  return 0;
}

int runProbeDir(const std::string& dir, const ProbeOptions& options) {
  ProbeContext ctx;
  std::string error;
  try {
    if (!prepare(options, ctx, error)) {
      spdlog::error("{}", error);
      return 1;
    }

    std::unique_ptr<ReplaySource> source = ReplaySource::open(dir, error);
    if (!source) {
      spdlog::error("{}", error);
      return 1;
    }

    CropSelectorParams params;
    params.require_liveness = ctx.config.ir_liveness.enabled;
    params.gain = {ctx.config.ir_liveness.min_face_gain, ctx.config.ir_liveness.min_gain_ratio};
    CropSelector selector(params);

    FrameAnalyzer analyzer(*ctx.models.detector, ctx.models.recognizer.get());
    std::vector<Observation> observations;
    auto handle = [&](const std::vector<Observation>& batch) {
      for (const Observation& obs : batch) {
        printLine(describe(obs, ctx));
        if (!options.save_crops.empty() && selector.accept(obs)) {
          writeGreyPng(fmt::format("{}/{}.png", options.save_crops, obs.timestamp_us / 1000),
                       obs.crop);
        }
      }
    };

    Frame frame;
    while (source->read(frame, 0) == ReadStatus::kOk) {
      observations.clear();
      analyzer.push(std::move(frame), observations);
      handle(observations);
    }
    observations.clear();
    analyzer.flush(observations);
    handle(observations);
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }
  return 0;
}

}  // namespace lhc
