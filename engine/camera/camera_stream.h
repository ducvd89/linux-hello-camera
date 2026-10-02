#pragma once

#include <functional>
#include <memory>
#include <string>

#include "camera/frame_source.h"

namespace lhc {

// A camera that heals itself: when it starts returning flat frames it is
// reopened, so callers just see a stream of good frames.
class CameraStream {
 public:
  // Opens a source, or returns null with the reason in `error`.
  using Opener = std::function<std::unique_ptr<FrameSource>(std::string& error)>;

  // A V4L2 camera at `path`.
  explicit CameraStream(const std::string& path);
  explicit CameraStream(Opener opener) : opener_(std::move(opener)) {}

  bool open(std::string& error);

  // The next non-uniform frame, waiting up to `timeout_ms` in total.
  ReadStatus next(Frame& out, int timeout_ms);

  // Frees the device so another process can use it.
  void close() { camera_.reset(); }

  int reopenCount() const { return reopen_count_; }

 private:
  Opener opener_;
  std::unique_ptr<FrameSource> camera_;
  int reopen_count_ = 0;
  uint32_t next_seq_ = 0;
};

}  // namespace lhc
