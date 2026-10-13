#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "camera/camera_stream.h"
#include "camera/frame.h"

namespace lhc {

// Reads a second camera on its own thread and keeps only its newest frame, so a loop driven by
// the IR camera can look at what the other camera saw at about the same moment without either
// camera's queue backing up.
class FrameGrabber {
 public:
  // Starts reading `stream`, which must be open and must outlive the grabber.
  explicit FrameGrabber(CameraStream& stream);
  ~FrameGrabber();
  FrameGrabber(const FrameGrabber&) = delete;
  FrameGrabber& operator=(const FrameGrabber&) = delete;

  // The newest frame not taken before; false if there is none yet.
  bool take(Frame& out);

  // The camera stopped delivering (a read error); take() then only returns what came before.
  bool failed() const { return failed_; }

 private:
  void run();

  CameraStream& stream_;
  std::mutex mutex_;
  Frame latest_;
  bool fresh_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::thread thread_;
};

}  // namespace lhc
