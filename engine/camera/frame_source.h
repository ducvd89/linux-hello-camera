#pragma once

#include "camera/frame.h"

namespace lhc {

enum class ReadStatus { kOk, kTimeout, kError };

// Anything that produces frames; the V4L2 camera, or a fake in tests.
class FrameSource {
 public:
  virtual ~FrameSource() = default;

  // Waits up to `timeout_ms` for the next frame.
  virtual ReadStatus read(Frame& out, int timeout_ms) = 0;
};

}  // namespace lhc
