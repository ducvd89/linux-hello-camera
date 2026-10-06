#include "camera/frame.h"

#include <algorithm>
#include <cstdlib>

namespace lhc {

bool isUniform(const GreyImage& image) {
  if (image.empty()) {
    return false;
  }
  const auto [lo, hi] = std::minmax_element(image.data.begin(), image.data.end());
  return *lo == *hi;
}

double meanBrightness(const GreyImage& image) {
  if (image.empty()) {
    return 0.0;
  }
  uint64_t sum = 0;
  for (uint8_t v : image.data) {
    sum += v;
  }
  return static_cast<double>(sum) / static_cast<double>(image.data.size());
}

std::vector<bool> classifyLit(const std::vector<double>& means) {
  std::vector<bool> lit(means.size());
  for (size_t i = 0; i < means.size(); ++i) {
    const size_t first = i > kLitWindowRadius ? i - kLitWindowRadius : 0;
    const size_t last = std::min(means.size() - 1, i + kLitWindowRadius);
    const auto [lo, hi] =
        std::minmax_element(means.begin() + static_cast<std::ptrdiff_t>(first),
                            means.begin() + static_cast<std::ptrdiff_t>(last) + 1);
    if (*hi - *lo < kMinStrobeContrast) {
      lit[i] = isLit(means[i]);
    } else {
      lit[i] = means[i] > (*hi + *lo) / 2.0;
    }
  }
  return lit;
}

int findUnlitPartner(const std::vector<FrameStamp>& frames, size_t lit_index, int64_t max_gap_us) {
  if (lit_index >= frames.size()) {
    return -1;
  }
  int best = -1;
  int64_t best_gap = max_gap_us + 1;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i].lit) {
      continue;
    }
    const int64_t gap = std::llabs(frames[i].timestamp_us - frames[lit_index].timestamp_us);
    // Strict < keeps the earlier frame on a tie.
    if (gap <= max_gap_us && gap < best_gap) {
      best = static_cast<int>(i);
      best_gap = gap;
    }
  }
  return best;
}

}  // namespace lhc
