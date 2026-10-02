#pragma once

#include <deque>
#include <vector>

#include "analysis/observation.h"
#include "camera/frame.h"
#include "face/detection/face_detection.h"
#include "face/recognition/face_recognition.h"

namespace lhc {

// Turns a stream of frames into observations of the lit ones. A lit frame is
// analysed one frame after it arrives so that both its neighbours are known:
// its unlit partner can be the previous or the next frame.
class FrameAnalyzer {
 public:
  // `recognizer` may be null when only liveness is of interest.
  FrameAnalyzer(FaceDetection& detector, FaceRecognition* recognizer)
      : detector_(detector), recognizer_(recognizer) {}

  // Feeds the next frame; appends an observation for every lit frame that became ready.
  void push(Frame frame, std::vector<Observation>& out);

  // At the end of a recording: analyses a lit frame that is still waiting for its successor.
  void flush(std::vector<Observation>& out);

 private:
  struct Entry {
    Frame frame;
    bool lit;
    bool analysed;
  };

  void analyseIfReady(size_t index, std::vector<Observation>& out);

  FaceDetection& detector_;
  FaceRecognition* recognizer_;
  std::deque<Entry> history_;
};

}  // namespace lhc
