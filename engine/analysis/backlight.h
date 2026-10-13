#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

#include "analysis/authenticator.h"
#include "analysis/models.h"
#include "camera/camera_stream.h"
#include "core/config.h"

namespace lhc {

// Backlight fallback (DESIGN.md). With a bright window behind the user the IR camera exposes for
// the window: the face comes out nearly black and the emitter brightens it by a few grey levels
// instead of the tens measureGain needs. The colour camera, which controls its own exposure, still
// sees the face well. Then a face counts only when, at about the same moment and place:
//  - the colour camera sees it, it matches the user's colour pictures and the anti-spoofing
//    model calls it real, and
//  - the IR camera sees it (brightened for detection), with both eyes glinting in the lit frame
//    and not in the unlit one, and the face still brightening a little.
// A phone or monitor is black to the IR camera; a matte print has no glints and fails the
// anti-spoofing model.

// The IR face must still brighten this much (grey levels) when the emitter fires.
inline constexpr float kBacklightMinFaceGain = 3.0f;

// The two cameras sit side by side and see the face at nearly the same place in their frames
// (measured: centres within 0.04 of the width). Further apart than this, in either direction, as a
// fraction of the frame, and they are not looking at the same face.
inline constexpr float kMaxCentreOffset = 0.15f;

// Sightings further apart in time than this do not confirm each other.
inline constexpr int64_t kMaxSightingGapUs = 500000;

// One look by either camera. The centre is a fraction of the frame's width and height.
struct Sighting {
  int64_t time_us = 0;  // kernel capture time (CLOCK_MONOTONIC) of the frame
  bool good = false;    // colour: matched and real; IR: live (glints and some brightening)
  float cx = 0.0f;
  float cy = 0.0f;
};

// Succeeds when, among the last max(3, needed) looks of each camera, `colour_needed` colour looks
// and `ir_needed` IR looks were good, and a good colour look and a good IR look were taken within
// kMaxSightingGapUs of each other with centres within kMaxCentreOffset.
class BacklightTracker {
 public:
  BacklightTracker(int colour_needed, int ir_needed);

  void addColour(const Sighting& s) { add(colour_, colour_window_, s); }
  void addIr(const Sighting& s) { add(ir_, ir_window_, s); }
  bool satisfied() const;

 private:
  static void add(std::deque<Sighting>& recent, size_t window, const Sighting& s);

  int colour_needed_;
  int ir_needed_;
  size_t colour_window_;
  size_t ir_window_;
  std::deque<Sighting> colour_;
  std::deque<Sighting> ir_;
};

// Whether two looks (one per camera) show the same face: close in time and place.
bool sameFace(const Sighting& colour, const Sighting& ir);

// The fallback itself: reads `ir` and `colour` (both open) until the tracker is satisfied or
// `deadline` passes. `enrolled` are the user's colour embeddings; `start` is when the request
// began, for elapsed_ms. models.colour_antispoof must be loaded.
AuthReport runBacklightAuth(const Config& config, CameraStream& ir, CameraStream& colour,
                            Models& models, const std::vector<std::vector<float>>& enrolled,
                            std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point deadline);

}  // namespace lhc
