#include "analysis/backlight.h"

#include <algorithm>
#include <cstdlib>

#include "analysis/frame_analyzer.h"
#include "camera/frame_grabber.h"

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

Sighting sightingOf(int64_t time_us, bool good, const Box& box, int width, int height) {
  Sighting s;
  s.time_us = time_us;
  s.good = good;
  s.cx = (box.x1 + box.x2) / 2.0f / static_cast<float>(std::max(1, width));
  s.cy = (box.y1 + box.y2) / 2.0f / static_cast<float>(std::max(1, height));
  return s;
}

}  // namespace

BacklightTracker::BacklightTracker(int colour_needed, int ir_needed)
    : colour_needed_(std::max(1, colour_needed)),
      ir_needed_(std::max(1, ir_needed)),
      colour_window_(static_cast<size_t>(std::max(3, colour_needed_))),
      ir_window_(static_cast<size_t>(std::max(3, ir_needed_))) {}

void BacklightTracker::add(std::deque<Sighting>& recent, size_t window, const Sighting& s) {
  recent.push_back(s);
  if (recent.size() > window) {
    recent.pop_front();
  }
}

bool sameFace(const Sighting& colour, const Sighting& ir) {
  return std::llabs(colour.time_us - ir.time_us) <= kMaxSightingGapUs &&
         std::abs(colour.cx - ir.cx) <= kMaxCentreOffset &&
         std::abs(colour.cy - ir.cy) <= kMaxCentreOffset;
}

bool BacklightTracker::satisfied() const {
  auto good = [](const Sighting& s) { return s.good; };
  if (std::count_if(colour_.begin(), colour_.end(), good) < colour_needed_ ||
      std::count_if(ir_.begin(), ir_.end(), good) < ir_needed_) {
    return false;
  }
  for (const Sighting& c : colour_) {
    for (const Sighting& i : ir_) {
      if (c.good && i.good && sameFace(c, i)) {
        return true;
      }
    }
  }
  return false;
}

AuthReport runBacklightAuth(const Config& config, CameraStream& ir, CameraStream& colour,
                            Models& models, const std::vector<std::vector<float>>& enrolled,
                            std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point deadline) {
  using Clock = std::chrono::steady_clock;
  FrameAnalyzer analyzer(*models.detector, nullptr);  // IR: only where the face is, and liveness
  BacklightTracker tracker(config.recognition.frames_needed,
                           std::max(1, config.ir_liveness.min_pairs));
  FrameGrabber grabber(colour);

  AuthReport report;
  report.used_colour = true;
  report.best_score = -1.0f;
  int ir_frames = 0, colour_frames = 0, colour_faces = 0;
  bool matched_but_not_good = false;
  bool succeeded = false;
  std::vector<float> face_gains, ratios;

  auto observeIr = [&](const Observation& obs, int width, int height) {
    if (!obs.face_found) {
      tracker.addIr({obs.timestamp_us, false, 0.0f, 0.0f});
      return;
    }
    if (obs.paired) {
      face_gains.push_back(obs.gain.face_gain);
      ratios.push_back(obs.gain.ratio);
    }
    const bool live = obs.paired && glintsPass(obs.glints) &&
                      obs.gain.face_gain >= kBacklightMinFaceGain &&
                      obs.gain.ratio >= config.ir_liveness.min_gain_ratio;
    report.liveness_pairs += live ? 1 : 0;
    tracker.addIr(sightingOf(obs.timestamp_us, live, obs.box, width, height));
  };

  auto observeColour = [&](const Frame& frame) {
    ++colour_frames;
    const std::vector<Detection> faces = models.detector->inference(frame.colour);
    if (faces.empty()) {
      tracker.addColour({frame.timestamp_us, false, 0.0f, 0.0f});
      return;
    }
    ++colour_faces;
    const Detection& face = faces.front();  // largest
    const float score = bestScore(models.recognizer->embed(face.image), enrolled);
    report.best_score = std::max(report.best_score, score);
    const bool matched = score >= config.recognition.threshold;
    // Without the model nothing vouches for the colour picture being a real face.
    const bool real =
        models.colour_antispoof && !models.colour_antispoof->inference(face.image).spoof;
    matched_but_not_good = matched_but_not_good || (matched && !real);
    tracker.addColour(sightingOf(frame.timestamp_us, matched && real, face.box,
                                 frame.colour.width, frame.colour.height));
  };

  std::vector<Observation> observations;
  while (!succeeded) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) {
      break;
    }
    Frame frame;
    const ReadStatus status = ir.next(frame, static_cast<int>(left));
    if (status == ReadStatus::kError) {
      break;
    }
    if (status == ReadStatus::kTimeout) {
      continue;
    }
    ++ir_frames;
    const int width = frame.image.width, height = frame.image.height;
    observations.clear();
    analyzer.push(std::move(frame), observations);
    for (const Observation& obs : observations) {
      observeIr(obs, width, height);
    }
    // The colour camera gets the turns the IR camera leaves free (a frame that brought no IR
    // analysis): doing both on one frame takes longer than the next frame takes to arrive, frames
    // get dropped, and lit frames lose their unlit partners.
    Frame colour_frame;
    if (observations.empty() && grabber.take(colour_frame) && !colour_frame.colour.empty()) {
      observeColour(colour_frame);
    }
    succeeded = tracker.satisfied();
  }

  report.elapsed_ms = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
  report.best_score = std::max(report.best_score, 0.0f);
  report.face_gain_median = medianOf(face_gains);
  report.ratio_median = medianOf(ratios);

  if (succeeded) {
    report.verdict = Verdict::kOk;
  } else if (ir_frames == 0 || colour_frames == 0) {
    report.verdict = Verdict::kCameraUnavailable;
    report.detail = ir_frames == 0 ? "no IR frames" : "no colour frames";
  } else if (colour_faces == 0) {
    report.verdict = Verdict::kNoFace;
  } else if (matched_but_not_good || report.best_score >= config.recognition.threshold) {
    // Matched in colour but not confirmed as real (anti-spoofing or IR).
    report.verdict = Verdict::kLivenessFailed;
  } else {
    report.verdict = Verdict::kNotRecognised;
  }
  return report;
}

}  // namespace lhc
