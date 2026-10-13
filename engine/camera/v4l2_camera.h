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

  // Opens `path`, picks GREY, else YUYV, else MJPEG, and starts streaming. With `colour`, asks for
  // kColourWidth x kColourHeight MJPEG, else YUYV, and fills Frame::colour as well.
  static std::unique_ptr<V4l2Camera> open(const std::string& path, std::string& error,
                                          bool colour = false);

  // A colour camera shares the USB link with the IR one; this size in MJPEG leaves room for both.
  static constexpr int kColourWidth = 640;
  static constexpr int kColourHeight = 480;

  // YUYV and MJPEG frames are reduced to their luma (and decoded to RGB in colour mode).
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
  bool convertColour(const Buffer& buffer, size_t bytes_used, Frame& out);

  bool colour_ = false;
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
