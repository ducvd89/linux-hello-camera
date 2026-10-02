#include "analysis/match_tracker.h"

#include <algorithm>

namespace lhc {

MatchTracker::MatchTracker(int frames_needed, int min_pairs)
    : needed_(std::max({1, frames_needed, min_pairs})),
      window_(static_cast<size_t>(std::max(3, needed_))) {}

void MatchTracker::add(bool good) {
  recent_.push_back(good);
  if (recent_.size() > window_) {
    recent_.pop_front();
  }
}

bool MatchTracker::satisfied() const {
  return std::count(recent_.begin(), recent_.end(), true) >= needed_;
}

}  // namespace lhc
