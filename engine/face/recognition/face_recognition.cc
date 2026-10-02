#include "face/recognition/face_recognition.h"

#include <cmath>

namespace lhc {

float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) {
    return 0.0f;
  }
  float dot = 0, norm_a = 0, norm_b = 0;
  for (size_t i = 0; i < a.size(); i++) {
    dot += a[i] * b[i];
    norm_a += a[i] * a[i];
    norm_b += b[i] * b[i];
  }
  if (norm_a == 0 || norm_b == 0) {
    return 0.0f;
  }
  return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
}

FaceRecognition::FaceRecognition(const std::string& ckpt, int imgsz)
    : imgsz(imgsz), session(ckpt, "FaceRecognition") {}

std::vector<float> FaceRecognition::preprocess(const ImageRGB& input_image) {
  ImageRGB resize_img = imageResizePad(input_image, this->imgsz, this->imgsz);

  const float mean[3] = {0.5f, 0.5f, 0.5f};
  const float std[3] = {0.5f, 0.5f, 0.5f};

  return imageToChwNormalized(resize_img, mean, std);
}

std::vector<float> FaceRecognition::embed(const ImageRGB& image) {
  std::vector<float> input_data = this->preprocess(image);

  std::vector<int64_t> input_shape = {1, 3, (int64_t)this->imgsz, (int64_t)this->imgsz};
  auto output_tensors = this->session.run(input_data, input_shape);

  auto& out = output_tensors[0];
  auto shape = out.GetTensorTypeAndShapeInfo().GetShape();
  int embed_dim = static_cast<int>(shape[1]);
  const float* data = out.GetTensorData<float>();

  return std::vector<float>(data, data + embed_dim);
}

}  // namespace lhc
