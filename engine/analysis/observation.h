#pragma once

#include <cstdint>
#include <vector>

#include "core/geometry.h"
#include "face/image_utils.h"
#include "liveness/liveness.h"

namespace lhc {

// What the engine learned from one lit frame: the largest face in it, its
// embedding, and the strobe response against the neighbouring unlit frame.
struct Observation {
  uint32_t lit_seq = 0;
  int64_t timestamp_us = 0;
  double lit_mean = 0.0;

  bool face_found = false;
  float det_score = 0.0f;
  Box box;
  Landmarks landmarks;
  ImageRGB crop;                 // detection box, grey replicated to 3 channels
  std::vector<float> embedding;  // empty without a face or a recognizer

  bool paired = false;  // an unlit partner was found
  uint32_t unlit_seq = 0;
  int64_t pair_gap_us = 0;
  GainResult gain;
  int specular = 0;  // near-saturated pixels in the eye regions (lit frame)
};

}  // namespace lhc
