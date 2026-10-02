#pragma once

#include <linux/videodev2.h>

#include <memory>
#include <string>
#include <vector>

#include "camera/frame.h"
#include "camera/frame_source.h"

namespace lhc {

// Minimal V4L2 capture: mmap streaming, one grey frame per read(). Only one
// process can open the camera at a time, so keep this object short-lived.
class V4l2Camera : public FrameSource {
 public:
  ~V4l2Camera() override;
  V4l2Camera(const V4l2Camera&) = delete;
  V4l2Camera& operator=(const V4l2Camera&) = delete;

  // Opens `path`, picks GREY, else YUYV, else MJPEG, and starts streaming.
  static std::unique_ptr<V4l2Camera> open(const std::string& path, std::string& error);

  // YUYV and MJPEG frames are reduced to their luma.
  ReadStatus read(Frame& out, int timeout_ms) override;

 private:
  struct Buffer {
    void* start = nullptr;
    size_t length = 0;
  };

  V4l2Camera() = default;
  bool init(const std::string& path, std::string& error);
  bool chooseFormat(std::string& error);
  bool startStreaming(std::string& error);
  bool convert(const Buffer& buffer, size_t bytes_used, GreyImage& out);

  int fd_ = -1;
  uint32_t pixel_format_ = 0;
  int width_ = 0;
  int height_ = 0;
  size_t stride_ = 0;
  bool streaming_ = false;
  std::vector<Buffer> buffers_;
  void* tj_ = nullptr;  // turbojpeg handle, MJPEG only
};

}  // namespace lhc
