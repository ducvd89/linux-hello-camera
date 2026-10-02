#include "analysis/authenticator.h"

#include <algorithm>

#include "analysis/frame_analyzer.h"
#include "analysis/match_tracker.h"

namespace lhc {

namespace {

float bestScore(const std::vector<float>& embedding,
                const std::vector<std::vector<float>>& enrolled) {
  float best = -1.0f;
  for (const auto& e : enrolled) {
    best = std::max(best, cosineSimilarity(embedding, e));
  }
  return best;
}

}  // namespace

const char* verdictName(Verdict verdict) {
  switch (verdict) {
    case Verdict::kOk:
      return "ok";
    case Verdict::kNotRecognised:
      return "not_recognised";
    case Verdict::kNoFace:
      return "no_face";
    case Verdict::kLivenessFailed:
      return "liveness_failed";
    case Verdict::kNotEnrolled:
      return "not_enrolled";
    case Verdict::kCameraUnavailable:
      return "camera_unavailable";
    case Verdict::kTooDark:
      return "too_dark";
    case Verdict::kError:
      return "error";
  }
  return "camera_unavailable";
}

float medianOf(std::vector<float> values) {
  if (values.empty()) {
    return 0.0f;
  }
  std::sort(values.begin(), values.end());
  const size_t mid = values.size() / 2;
  return values.size() % 2 == 1 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0f;
}

AuthReport runAuth(const Config& config, CameraStream& camera, Models& models,
                   const std::vector<std::vector<float>>& enrolled,
                   std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
  const bool liveness_on = config.ir_liveness.enabled;
  const GainParams gain_params{config.ir_liveness.min_face_gain, config.ir_liveness.min_gain_ratio};

  FrameAnalyzer analyzer(*models.detector, models.recognizer.get());
  MatchTracker tracker(config.recognition.frames_needed,
                       liveness_on ? config.ir_liveness.min_pairs : 0);

  AuthReport report;
  report.best_score = -1.0f;
  int frames = 0, lit_frames = 0, faces = 0;
  bool matched_but_not_good = false;
  bool succeeded = false;
  std::vector<float> face_gains, ratios;

  auto observe = [&](const Observation& obs) {
    ++lit_frames;
    if (!obs.face_found) {
      tracker.add(false);
      return;
    }
    ++faces;
    const bool live_pair = obs.paired && gainPasses(obs.gain, gain_params);
    if (obs.paired) {
      face_gains.push_back(obs.gain.face_gain);
      ratios.push_back(obs.gain.ratio);
    }
    if (live_pair) {
      ++report.liveness_pairs;
    }

    const float score = bestScore(obs.embedding, enrolled);
    report.best_score = std::max(report.best_score, score);
    const bool matched = score >= config.recognition.threshold;
    bool good = matched && (!liveness_on || live_pair);
    if (good && models.antispoof) {
      good = !models.antispoof->inference(obs.crop).spoof;
    }
    matched_but_not_good = matched_but_not_good || (matched && !good);
    tracker.add(good);
    succeeded = succeeded || tracker.satisfied();
  };

  std::vector<Observation> observations;
  while (!succeeded) {
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
    ++frames;
    observations.clear();
    analyzer.push(std::move(frame), observations);
    for (const Observation& obs : observations) {
      observe(obs);
      if (succeeded) {
        break;
      }
    }
  }

  report.elapsed_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
  report.best_score = std::max(report.best_score, 0.0f);
  report.face_gain_median = medianOf(face_gains);
  report.ratio_median = medianOf(ratios);

  if (succeeded) {
    report.verdict = Verdict::kOk;
  } else if (frames == 0) {
    report.verdict = Verdict::kCameraUnavailable;
  } else if (lit_frames == 0) {
    report.verdict = Verdict::kTooDark;
  } else if (faces == 0) {
    report.verdict = Verdict::kNoFace;
  } else if (matched_but_not_good) {
    report.verdict = Verdict::kLivenessFailed;
  } else {
    report.verdict = Verdict::kNotRecognised;
  }
  return report;
}

}  // namespace lhc
