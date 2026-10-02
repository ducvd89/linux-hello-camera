#include "analysis/crop_selector.h"

#include <cstdlib>

#include "face/recognition/face_recognition.h"

namespace lhc {

bool CropSelector::accept(const Observation& obs) {
  if (!obs.face_found || obs.embedding.empty() || obs.det_score < params_.min_det_score) {
    return false;
  }
  if (params_.require_liveness && !(obs.paired && gainPasses(obs.gain, params_.gain))) {
    return false;
  }
  for (int64_t t : times_us_) {
    if (std::llabs(obs.timestamp_us - t) < params_.min_spacing_us) {
      return false;
    }
  }
  for (const auto& e : embeddings_) {
    if (cosineSimilarity(obs.embedding, e) > params_.max_similarity) {
      return false;
    }
  }
  times_us_.push_back(obs.timestamp_us);
  embeddings_.push_back(obs.embedding);
  return true;
}

}  // namespace lhc
