#ifndef FACE_REG_H
#define FACE_REG_H

#include <string>
#include <vector>

#include "face/common/onnx_session.h"
#include "face/image_utils.h"

namespace lhc {

// Cosine similarity of two embeddings; 0 if they differ in length or either has no magnitude.
float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

// EdgeFace: face crop -> embedding. Callers compare embeddings with cosineSimilarity() and apply
// their own threshold.
class FaceRecognition {
 public:
  FaceRecognition(const std::string& ckpt, int imgsz = 112);

  std::vector<float> embed(const ImageRGB& image);

 private:
  std::vector<float> preprocess(const ImageRGB& image);

  int imgsz;
  OnnxSession session;
};

}  // namespace lhc

#endif  // FACE_REG_H
