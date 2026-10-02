#include "camera/camera_stream.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "camera/v4l2_camera.h"

namespace lhc {

CameraStream::CameraStream(const std::string& path)
    : opener_([path](std::string& error) -> std::unique_ptr<FrameSource> {
        return V4l2Camera::open(path, error);
      }) {}

bool CameraStream::open(std::string& error) {
  camera_ = opener_(error);
  return camera_ != nullptr;
}

ReadStatus CameraStream::next(Frame& out, int timeout_ms) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));

  for (;;) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();

    if (camera_ == nullptr) {
      // Reopening right after a close can briefly fail while the kernel tears
      // the old stream down.
      std::string error;
      if (!open(error)) {
        if (left <= 0) {
          return ReadStatus::kError;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(50, left)));
        continue;
      }
    }

    const ReadStatus status = camera_->read(out, static_cast<int>(std::max<int64_t>(left, 0)));
    if (status != ReadStatus::kOk) {
      return status;
    }
    if (!isUniform(out.image)) {
      out.seq = next_seq_++;
      return ReadStatus::kOk;
    }
    ++reopen_count_;
    spdlog::info("camera returned a flat frame, reopening");
    camera_.reset();
  }
}

}  // namespace lhc
