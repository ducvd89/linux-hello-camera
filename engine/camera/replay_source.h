#pragma once

#include <memory>
#include <string>
#include <vector>

#include "camera/frame_source.h"

namespace lhc {

// Plays back a recorded session folder (PNG frames + session.json with per-frame "file" and "t"
// seconds, optional "seq"), as the frames a camera would deliver, without waiting. After the last
// frame read() waits out its timeout (at most a moment) and reports a timeout. Serves probe-dir and
// the tests.
class ReplaySource : public FrameSource {
 public:
  // Null with `error` set if the folder has no readable session.json.
  static std::unique_ptr<ReplaySource> open(const std::string& dir, std::string& error);

  ReadStatus read(Frame& out, int timeout_ms) override;

 private:
  struct Item {
    std::string path;
    int64_t timestamp_us;
    uint32_t seq;
  };

  std::vector<Item> items_;
  size_t next_ = 0;
};

}  // namespace lhc
