#include "liveness/liveness.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lhc {

namespace {

Box clipTo(const Box& box, int width, int height) {
  return Box(std::max(0, box.x1), std::max(0, box.y1), std::min(width, box.x2),
             std::min(height, box.y2));
}

Box scaleAboutCentre(const Box& box, float scale) {
  const float cx = (box.x1 + box.x2) / 2.0f;
  const float cy = (box.y1 + box.y2) / 2.0f;
  const float hw = box.width() * scale / 2.0f;
  const float hh = box.height() * scale / 2.0f;
  return Box(static_cast<int>(std::floor(cx - hw)), static_cast<int>(std::floor(cy - hh)),
             static_cast<int>(std::ceil(cx + hw)), static_cast<int>(std::ceil(cy + hh)));
}

constexpr int kDiffBins = 511;  // lit - unlit lies in [-255, 255]

}  // namespace

GainResult measureGain(const GreyImage& lit, const GreyImage& unlit, const Box& face_box) {
  GainResult result;
  if (lit.empty() || lit.width != unlit.width || lit.height != unlit.height) {
    return result;
  }
  const Box face = clipTo(face_box, lit.width, lit.height);
  if (face.area() == 0) {
    return result;
  }
  const Box excluded =
      clipTo(scaleAboutCentre(face, kBackgroundExclusionScale), lit.width, lit.height);

  int64_t face_sum = 0;
  std::array<int, kDiffBins> background_hist{};
  int background_pixels = 0;

  for (int y = 0; y < lit.height; ++y) {
    const bool row_in_excluded = y >= excluded.y1 && y < excluded.y2;
    const bool row_in_face = y >= face.y1 && y < face.y2;
    for (int x = 0; x < lit.width; ++x) {
      const int diff = static_cast<int>(lit.at(x, y)) - static_cast<int>(unlit.at(x, y));
      if (row_in_face && x >= face.x1 && x < face.x2) {
        face_sum += diff;
      }
      if (!(row_in_excluded && x >= excluded.x1 && x < excluded.x2)) {
        ++background_hist[diff + 255];
        ++background_pixels;
      }
    }
  }

  result.face_gain = static_cast<float>(face_sum) / static_cast<float>(face.area());

  if (background_pixels >= kMinBackgroundPixels) {
    // Median from the histogram: first bin where the running count passes half.
    const int half = (background_pixels + 1) / 2;
    int seen = 0;
    for (int bin = 0; bin < kDiffBins; ++bin) {
      seen += background_hist[bin];
      if (seen >= half) {
        result.bg_gain = static_cast<float>(bin - 255);
        break;
      }
    }
  }

  result.ratio = result.face_gain / std::max(result.bg_gain, 1.0f);
  return result;
}

bool gainPasses(const GainResult& gain, const GainParams& params) {
  return gain.face_gain >= params.min_face_gain && gain.ratio >= params.min_gain_ratio;
}

std::vector<Box> eyeRegions(const Box& face, const Landmarks& landmarks) {
  if (!landmarks.valid) {
    return {Box(face.x1, face.y1, face.x2, face.y1 + face.height() / 3)};
  }
  const int half = std::max(1, static_cast<int>(std::lround(0.12f * face.width())));
  std::vector<Box> regions;
  for (int eye : {Landmarks::kLeftEye, Landmarks::kRightEye}) {
    const int cx = static_cast<int>(std::lround(landmarks.x[eye]));
    const int cy = static_cast<int>(std::lround(landmarks.y[eye]));
    regions.emplace_back(cx - half, cy - half, cx + half, cy + half);
  }
  return regions;
}

int countSpecular(const GreyImage& lit, const std::vector<Box>& regions) {
  int count = 0;
  for (const Box& region : regions) {
    const Box r = clipTo(region, lit.width, lit.height);
    for (int y = r.y1; y < r.y2; ++y) {
      for (int x = r.x1; x < r.x2; ++x) {
        if (lit.at(x, y) >= kSpecularLevel) {
          ++count;
        }
      }
    }
  }
  return count;
}

}  // namespace lhc
