#pragma once

#include <cstdint>
#include <vector>

#include "analysis/observation.h"

namespace lhc {

struct CropSelectorParams {
  float min_det_score = 0.7f;
  int64_t min_spacing_us = 300000;  // crops come from distinct moments
  float max_similarity = 0.97f;     // and must not be near-duplicates
  bool require_liveness = true;
  GainParams gain;
};

// Picks which observations are worth saving as enrolled crops.
class CropSelector {
 public:
  explicit CropSelector(const CropSelectorParams& params) : params_(params) {}

  // True (and the observation is remembered) if it should be saved.
  bool accept(const Observation& obs);

 private:
  CropSelectorParams params_;
  std::vector<int64_t> times_us_;
  std::vector<std::vector<float>> embeddings_;
};

}  // namespace lhc
