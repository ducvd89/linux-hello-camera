#include "analysis/frame_analyzer.h"

namespace lhc {

namespace {

// Enough to find a partner within kMaxPairGapUs at any plausible frame rate.
constexpr size_t kHistoryFrames = 8;

}  // namespace

void FrameAnalyzer::push(Frame frame, std::vector<Observation>& out) {
  const bool lit = isLit(meanBrightness(frame.image));
  history_.push_back({std::move(frame), lit, false});
  if (history_.size() > kHistoryFrames) {
    history_.pop_front();
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
  obs.lit_mean = meanBrightness(entry.frame.image);

  const std::vector<Detection> faces = detector_.inference(toRgb(entry.frame.image));
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
    }
    obs.specular = countSpecular(entry.frame.image, eyeRegions(face.box, face.landmarks));
  }
  out.push_back(std::move(obs));
}

}  // namespace lhc
