#ifndef FACE_DET_H
#define FACE_DET_H

// CPP native
#include <string>
#include <vector>

#include "core/geometry.h"
#include "face/common/onnx_session.h"
#include "face/image_utils.h"

namespace lhc {

struct Detection {
  Box box;
  Landmarks landmarks;  // valid only if the model outputs keypoints
  ImageRGB image;       // the box cropped out of the frame
  float conf{0.0};

  Detection(float conf, Box box, const Landmarks& landmarks, const ImageRGB& image)
      : box(box), landmarks(landmarks), image(image), conf(conf) {}

  int area() const { return box.area(); }

  bool operator>(const Detection& obj) const { return area() > obj.area(); }

  bool operator<(const Detection& obj) const { return area() < obj.area(); }
};

// YOLOv8n-face. Returns faces largest first; the crop is the plain detection box (no landmark
// alignment), which is also what the recognizer is fed.
class FaceDetection {
 public:
  FaceDetection(const std::string& ckpt, int imgsz = 640, const float conf = 0.50,
                const float iou = 0.50);

  std::vector<Detection> inference(const ImageRGB& image);

 private:
  std::vector<float> preprocess(const ImageRGB& image);

  float conf;
  float iou;
  int imgsz;
  OnnxSession session;
};

}  // namespace lhc

#endif  // FACE_DET_H
