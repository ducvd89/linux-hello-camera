#ifndef FACE_DET_UTILS_H
#define FACE_DET_UTILS_H

// CPP native
#include <vector>

namespace lhc {

struct RawDet {
  float x1, y1, x2, y2, conf;
  int cls;
  // YOLOv8-face keypoints (left eye, right eye, nose, left mouth, right mouth) in the same
  // coordinates as the box; only set when the model outputs them.
  bool has_keypoints = false;
  float kp_x[5] = {};
  float kp_y[5] = {};
};

std::vector<RawDet> non_max_suppression(const float* output, int num_preds, int pred_dim,
                                        float conf_thres = 0.25, float iou_thres = 0.45,
                                        int max_det = 300);

void scale_boxes(const std::vector<int>& img1_shape, std::vector<RawDet>& dets,
                 const std::vector<int>& img0_shape);

}  // namespace lhc

#endif  // FACE_DET_UTILS_H
