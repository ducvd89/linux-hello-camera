#include "analysis/models.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <fstream>

#include "face/enrolled_faces.h"

namespace lhc {

namespace {

// Keeps only the first line of an exception message; ONNX Runtime's are long.
std::string firstLine(const std::exception& e) {
  std::string msg = e.what();
  return msg.substr(0, msg.find('\n'));
}

}  // namespace

bool loadModels(const Config& config, const std::string& models_dir, Models& models,
                std::string& error) {
  const std::string det_path = modelPath(models_dir, config.detection.model);
  const std::string rec_path = modelPath(models_dir, config.recognition.model);
  const std::string as_path = modelPath(models_dir, config.ai_antispoof.model);
  for (const std::string& path : {det_path, rec_path}) {
    if (path.empty() || !std::ifstream(path).good()) {
      error = "model not found: " + path;
      return false;
    }
  }
  if (config.ai_antispoof.enabled && (as_path.empty() || !std::ifstream(as_path).good())) {
    error = "model not found: " + as_path;
    return false;
  }

  try {
    models.detector = std::make_unique<FaceDetection>(det_path, 640, config.detection.threshold);
    models.recognizer = std::make_unique<FaceRecognition>(rec_path, 112);
    if (config.ai_antispoof.enabled) {
      // "auto": the anti-spoofing class picks MiniFAS or MobileNetV3 from the file name.
      models.antispoof =
          std::make_unique<FaceAntiSpoofing>(as_path, 128, config.ai_antispoof.threshold, "auto");
    }
  } catch (const std::exception& e) {
    error = "cannot load models: " + firstLine(e);
    return false;
  }
  return true;
}

std::vector<std::vector<float>> embedEnrolled(FaceRecognition& recognizer,
                                              const std::string& faces_dir) {
  std::vector<std::vector<float>> embeddings;
  for (const std::string& path : listFaces(faces_dir)) {
    const ImageRGB crop = readImage(path);
    if (crop.empty()) {
      spdlog::warn("cannot read enrolled crop {}", path);
      continue;
    }
    embeddings.push_back(recognizer.embed(crop));
  }
  return embeddings;
}

}  // namespace lhc
