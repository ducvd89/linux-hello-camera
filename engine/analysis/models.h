#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "face/antispoofing/face_as.h"
#include "face/detection/face_detection.h"
#include "face/recognition/face_recognition.h"

namespace lhc {

struct Models {
  std::unique_ptr<FaceDetection> detector;
  std::unique_ptr<FaceRecognition> recognizer;
  std::unique_ptr<FaceAntiSpoofing> antispoof;  // only when ai_antispoof.enabled
};

// Loads the models named in `config` from `models_dir`. On failure returns false with the
// reason in `error`.
bool loadModels(const Config& config, const std::string& models_dir, Models& models,
                std::string& error);

// Embeddings of the enrolled crops in `faces_dir`, computed once so every camera frame only
// costs one more embedding. Unreadable crops are skipped.
std::vector<std::vector<float>> embedEnrolled(FaceRecognition& recognizer,
                                              const std::string& faces_dir);

}  // namespace lhc
