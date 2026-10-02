#pragma once

#include <deque>

namespace lhc {

// Decides when enough recent lit frames were good. A frame is good when it
// matched the enrolled face and (if enabled) its IR strobe pair and AI check
// passed. Success needs `frames_needed` good frames, and `min_pairs` when
// liveness is on, among the last max(3, needed) lit frames; frames without a
// face count as not good.
class MatchTracker {
 public:
  MatchTracker(int frames_needed, int min_pairs);

  void add(bool good);
  bool satisfied() const;

 private:
  int needed_;
  size_t window_;
  std::deque<bool> recent_;
};

}  // namespace lhc
