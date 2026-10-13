#include "camera/frame_grabber.h"

namespace lhc {

namespace {

// Short enough that the destructor never waits long for the thread.
constexpr int kReadTimeoutMs = 100;

}  // namespace

FrameGrabber::FrameGrabber(CameraStream& stream) : stream_(stream), thread_([this] { run(); }) {}

FrameGrabber::~FrameGrabber() {
  stop_ = true;
  thread_.join();
}

bool FrameGrabber::take(Frame& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!fresh_) {
    return false;
  }
  out = std::move(latest_);
  fresh_ = false;
  return true;
}

void FrameGrabber::run() {
  while (!stop_) {
    Frame frame;
    const ReadStatus status = stream_.next(frame, kReadTimeoutMs);
    if (status == ReadStatus::kError) {
      failed_ = true;
      return;
    }
    if (status == ReadStatus::kOk) {
      std::lock_guard<std::mutex> lock(mutex_);
      latest_ = std::move(frame);
      fresh_ = true;
    }
  }
}

}  // namespace lhc
