#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "analysis/models.h"
#include "camera/camera_stream.h"
#include "core/config.h"

namespace lhc {

enum class Verdict {
  kOk,
  kNotRecognised,
  kNoFace,
  kLivenessFailed,
  kNotEnrolled,
  kCameraUnavailable,
  kTooDark,
  kError,  // models missing or broken
};

// The "result" strings of `test` (DESIGN.md).
const char* verdictName(Verdict verdict);

struct AuthReport {
  Verdict verdict = Verdict::kCameraUnavailable;
  float best_score = 0.0f;  // best cosine similarity seen on any lit frame
  int liveness_pairs = 0;   // lit/unlit pairs that passed the strobe check
  int elapsed_ms = 0;
  float face_gain_median = 0.0f;  // over all pairs with a face, for calibration
  float ratio_median = 0.0f;
  std::string detail;  // reason, for kError and kCameraUnavailable
};

// The core of auth/test: reads frames from `camera` until a match is confirmed or `deadline`
// passes; `start` is when the caller began, for elapsed_ms. `enrolled` are the embeddings of the
// user's crops.
AuthReport runAuth(const Config& config, CameraStream& camera, Models& models,
                   const std::vector<std::vector<float>>& enrolled,
                   std::chrono::steady_clock::time_point start,
                   std::chrono::steady_clock::time_point deadline);

// Median of `values` (mean of the middle two for an even count); 0 for none.
float medianOf(std::vector<float> values);

}  // namespace lhc
