#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/grey_image.h"

namespace lhc {

struct Frame {
  GreyImage image;
  int64_t timestamp_us = 0;  // kernel capture time (CLOCK_MONOTONIC), microseconds
  uint32_t seq = 0;          // frame counter, assigned by CameraStream (UVC reports 0 here)
};

// The camera's IR emitter strobes, so frames alternate between lit (a face
// near the camera is bright) and unlit (almost black, mean well below this).
inline constexpr double kLitMeanMin = 5.0;

// True if every pixel has the same value. This camera stops streaming after a
// few seconds and then outputs a flat frame (all 144) until it is reopened.
bool isUniform(const GreyImage& image);

double meanBrightness(const GreyImage& image);

inline bool isLit(double mean) { return mean >= kLitMeanMin; }

// A frame as the pairing logic sees it.
struct FrameStamp {
  int64_t timestamp_us;
  bool lit;
};

// Longest gap between a lit frame and its unlit partner. At 15 fps with the
// emitter strobing the neighbour is ~67 ms away; this tolerates a dropped frame.
inline constexpr int64_t kMaxPairGapUs = 150000;

// Index of the unlit frame closest in time to frames[lit_index], searching
// both directions, or -1 if none is within `max_gap_us`. Ties go to the
// earlier frame.
int findUnlitPartner(const std::vector<FrameStamp>& frames, size_t lit_index,
                     int64_t max_gap_us = kMaxPairGapUs);

}  // namespace lhc
