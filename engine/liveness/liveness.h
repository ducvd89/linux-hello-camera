#pragma once

#include <vector>

#include "core/geometry.h"
#include "core/grey_image.h"

namespace lhc {

// IR strobe liveness. A real face a few tens of centimetres from the emitter
// brightens far more when the emitter fires than the room behind it does; a
// photo or a phone/laptop screen shows little or no response. Everything here
// is pure so it can be tested without a camera.

struct GainResult {
  float face_gain = 0.0f;  // mean(face box, lit) - mean(face box, unlit)
  float bg_gain = 0.0f;    // median(lit - unlit) over the frame outside the enlarged face box
  float ratio = 0.0f;      // face_gain / max(bg_gain, 1)
};

struct GainParams {
  float min_face_gain = 25.0f;
  float min_gain_ratio = 1.8f;
};

// The background excludes `face` enlarged by this factor around its centre, so
// hair, neck and shoulders (which the emitter lights too) stay out of it.
inline constexpr float kBackgroundExclusionScale = 1.5f;

// Fewer background pixels than this and the median means nothing; bg_gain is
// then 0 and the ratio degenerates to the face gain.
inline constexpr int kMinBackgroundPixels = 2000;

// `lit` and `unlit` must have the same size. Returns all zeros otherwise, or
// when `face` is empty after clipping to the image.
GainResult measureGain(const GreyImage& lit, const GreyImage& unlit, const Box& face);

bool gainPasses(const GainResult& gain, const GainParams& params);

// Pixels at or above this count as specular (glint) in the lit frame.
inline constexpr int kSpecularLevel = 250;

// Eye regions of a face: a square around each eye landmark, or the upper third
// of the face box when the detector gave no landmarks.
std::vector<Box> eyeRegions(const Box& face, const Landmarks& landmarks);

// Near-saturated pixels of `lit` inside `regions` (clipped to the image).
// Reported for calibration; nothing gates on it.
int countSpecular(const GreyImage& lit, const std::vector<Box>& regions);

// Eye glints: the emitter's reflection on the cornea (or glasses) is a bright spot in each eye
// that is there in the lit frame and gone in the unlit one, even when the rest of the face is
// too dark for measureGain (a bright window behind the user). A matte print has none.
inline constexpr int kGlintLevel = 100;        // a glint in the lit frame reaches this (seen: 117-255)
inline constexpr int kGlintUnlitLevel = 64;    // and the unlit frame stays below this there (seen: 0-11)

struct GlintResult {
  int regions = 0;      // eye regions looked at
  int lit = 0;          // regions with a pixel >= kGlintLevel in the lit frame
  int unlit = 0;        // regions with a pixel >= kGlintUnlitLevel in the unlit frame
  int weakest_lit = 255;  // the dimmer eye's brightest pixel, lit frame (for calibration)
  int brightest_unlit = 0;  // the brighter eye's brightest pixel, unlit frame
};

GlintResult measureGlints(const GreyImage& lit, const GreyImage& unlit,
                          const std::vector<Box>& regions);

// Both eyes (two regions, so the detector gave landmarks) glint when lit and not when unlit.
bool glintsPass(const GlintResult& glints);

}  // namespace lhc
