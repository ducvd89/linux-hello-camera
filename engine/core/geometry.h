#pragma once

#include <algorithm>

namespace lhc {

// Axis-aligned box in pixel coordinates; x2/y2 are exclusive.
struct Box {
  int x1, y1, x2, y2;
  Box(int x1 = 0, int y1 = 0, int x2 = 0, int y2 = 0) : x1(x1), y1(y1), x2(x2), y2(y2) {}

  int width() const { return x2 - x1; }
  int height() const { return y2 - y1; }
  int area() const { return std::max(0, width()) * std::max(0, height()); }
};

// The five YOLOv8-face keypoints in image pixels, in the model's order.
struct Landmarks {
  enum { kLeftEye, kRightEye, kNose, kLeftMouth, kRightMouth, kCount };
  bool valid = false;
  float x[kCount] = {};
  float y[kCount] = {};
};

}  // namespace lhc
