#include "analysis/frame_analyzer.h"

namespace lhc {

namespace {

// Enough to find a partner within kMaxPairGapUs at any plausible frame rate.
constexpr size_t kHistoryFrames = 8;

// Detection tries the lit frame as captured and, failing that, brightened copies (a face in front
// of a bright window comes out nearly black). Liveness still measures the frames as captured, so
// brightening never makes a photo look more alive. 1 means as captured.
constexpr float kGammas[] = {1.0f, 0.5f, 0.35f};

}  // namespace

void FrameAnalyzer::push(Frame frame, std::vector<Observation>& out) {
  const double mean = meanBrightness(frame.image);
  history_.push_back({std::move(frame), mean, false, false});
  if (history_.size() > kHistoryFrames) {
    history_.pop_front();
  }
  // Reclassify the whole history: a frame's verdict can change once its next neighbour is known.
  std::vector<double> means;
  means.reserve(history_.size());
  for (const Entry& e : history_) {
    means.push_back(e.mean);
  }
  const std::vector<bool> lit = classifyLit(means);
  for (size_t i = 0; i < history_.size(); ++i) {
    history_[i].lit = lit[i];
  }
  if (history_.size() >= 2) {
    analyseIfReady(history_.size() - 2, out);
  }
}

void FrameAnalyzer::flush(std::vector<Observation>& out) {
  if (!history_.empty()) {
    analyseIfReady(history_.size() - 1, out);
  }
}

void FrameAnalyzer::analyseIfReady(size_t index, std::vector<Observation>& out) {
  Entry& entry = history_[index];
  if (!entry.lit || entry.analysed) {
    return;
  }
  entry.analysed = true;

  Observation obs;
  obs.lit_seq = entry.frame.seq;
  obs.timestamp_us = entry.frame.timestamp_us;
  obs.lit_mean = entry.mean;

  // The level that found the face last time goes first: each extra pass costs about a frame's
  // time, and falling behind the camera drops frames and breaks the lit/unlit rhythm.
  std::vector<float> order = {gamma_};
  for (float gamma : kGammas) {
    if (gamma != gamma_) {
      order.push_back(gamma);
    }
  }
  std::vector<Detection> faces;
  for (float gamma : order) {
    faces = detector_.inference(
        toRgb(gamma == 1.0f ? entry.frame.image : brighten(entry.frame.image, gamma)));
    if (!faces.empty()) {
      obs.boost_gamma = gamma_ = gamma;
      break;
    }
  }
  if (!faces.empty()) {
    const Detection& face = faces.front();  // largest
    obs.face_found = true;
    obs.det_score = face.conf;
    obs.box = face.box;
    obs.landmarks = face.landmarks;
    obs.crop = face.image;
    if (recognizer_ != nullptr) {
      obs.embedding = recognizer_->embed(face.image);
    }

    std::vector<FrameStamp> stamps;
    stamps.reserve(history_.size());
    for (const Entry& e : history_) {
      stamps.push_back({e.frame.timestamp_us, e.lit});
    }
    const int partner = findUnlitPartner(stamps, index);
    if (partner >= 0) {
      const Frame& unlit = history_[partner].frame;
      obs.paired = true;
      obs.unlit_seq = unlit.seq;
      obs.pair_gap_us = unlit.timestamp_us - entry.frame.timestamp_us;
      obs.gain = measureGain(entry.frame.image, unlit.image, face.box);
      obs.glints = measureGlints(entry.frame.image, unlit.image, eyeRegions(face.box, face.landmarks));
    }
    obs.specular = countSpecular(entry.frame.image, eyeRegions(face.box, face.landmarks));
  }
  out.push_back(std::move(obs));
}

}  // namespace lhc
