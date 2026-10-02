#ifndef FACE_AS_H
#define FACE_AS_H

#include <string>
#include <vector>

#include "face/common/onnx_session.h"
#include "face/image_utils.h"

namespace lhc {

struct SpoofResult {
  float score;
  bool spoof;
  SpoofResult(float score, bool spoof) : score(score), spoof(spoof) {}
};

// model_type: "minifasv2" or "mobilenetv3" (default)
class FaceAntiSpoofing {
 public:
  FaceAntiSpoofing(const std::string& ckpt, int imgsz = 128, const float threshold = 0.8,
                   const std::string& model_type = "mobilenetv3");

  SpoofResult inference(const ImageRGB& image);

 private:
  std::vector<float> preprocess(const ImageRGB& image);
  std::vector<float> preprocessMobileNetV3(const ImageRGB& image);
  std::vector<float> preprocessMiniFASv2(const ImageRGB& image);

  float threshold;
  int imgsz;
  std::string model_type;
  OnnxSession session;
};

}  // namespace lhc

#endif  // FACE_AS_H
