#include "liveness/liveness.h"
#include "tests/test.h"

using namespace lhc;

namespace {

// A 640x400 scene: `room` grey levels everywhere, `face_level` inside `face`.
GreyImage scene(uint8_t room, uint8_t face_level, const Box& face) {
  GreyImage img(640, 400, room);
  for (int y = face.y1; y < face.y2; ++y) {
    for (int x = face.x1; x < face.x2; ++x) {
      img.at(x, y) = face_level;
    }
  }
  return img;
}

const Box kFace(220, 100, 420, 320);
const GainParams kParams{25.0f, 1.8f};

void testRealFace() {
  // The emitter lifts the face by 60 and the room behind by 10.
  const GreyImage unlit = scene(2, 3, kFace);
  const GreyImage lit = scene(12, 63, kFace);
  const GainResult g = measureGain(lit, unlit, kFace);
  CHECK_NEAR(g.face_gain, 60.0, 1e-4);
  CHECK_NEAR(g.bg_gain, 10.0, 1e-4);
  CHECK_NEAR(g.ratio, 6.0, 1e-4);
  CHECK(gainPasses(g, kParams));
}

void testScreenOrPhoto() {
  // A flat reflector: the whole scene brightens about equally.
  const GreyImage unlit = scene(2, 3, kFace);
  const GreyImage lit = scene(32, 40, kFace);
  const GainResult g = measureGain(lit, unlit, kFace);
  CHECK_NEAR(g.face_gain, 37.0, 1e-4);
  CHECK_NEAR(g.bg_gain, 30.0, 1e-4);
  CHECK(g.ratio < 1.8f);
  CHECK(!gainPasses(g, kParams));

  // A screen shows no response at all.
  const GainResult none = measureGain(scene(2, 3, kFace), scene(2, 3, kFace), kFace);
  CHECK_NEAR(none.face_gain, 0.0, 1e-6);
  CHECK(!gainPasses(none, kParams));
}

void testThresholds() {
  GainResult g;
  g.face_gain = 24.9f;
  g.ratio = 5.0f;
  CHECK(!gainPasses(g, kParams));  // gain just short
  g.face_gain = 25.0f;
  g.ratio = 1.79f;
  CHECK(!gainPasses(g, kParams));  // ratio just short
  g.ratio = 1.8f;
  CHECK(gainPasses(g, kParams));  // both exactly at the limit
}

void testBackgroundIsRobust() {
  // A bright lamp in the background must not move the median.
  GreyImage unlit = scene(2, 3, kFace);
  GreyImage lit = scene(12, 63, kFace);
  for (int y = 0; y < 40; ++y) {
    for (int x = 0; x < 40; ++x) {
      lit.at(x, y) = 255;
    }
  }
  CHECK_NEAR(measureGain(lit, unlit, kFace).bg_gain, 10.0, 1e-4);

  // Hair and shoulders just outside the face box brighten too but sit inside the
  // enlarged exclusion zone, so they do not count as background either.
  GreyImage lit2 = scene(12, 63, kFace);
  for (int y = 90; y < 330; ++y) {
    for (int x = 200; x < 220; ++x) {
      lit2.at(x, y) = 90;
    }
  }
  CHECK_NEAR(measureGain(lit2, unlit, kFace).bg_gain, 10.0, 1e-4);
}

void testNegativeBackgroundAndEdgeCases() {
  // Background gets darker (noise or exposure change): ratio uses max(bg_gain, 1).
  const GreyImage unlit = scene(10, 10, kFace);
  const GreyImage lit = scene(8, 60, kFace);
  const GainResult g = measureGain(lit, unlit, kFace);
  CHECK_NEAR(g.bg_gain, -2.0, 1e-4);
  CHECK_NEAR(g.ratio, g.face_gain, 1e-4);

  // The face box may stick out of the frame.
  const GainResult clipped =
      measureGain(scene(12, 63, kFace), scene(2, 3, kFace), Box(-50, 100, 420, 500));
  CHECK(clipped.face_gain > 0.0f);

  // Mismatched sizes, empty boxes and empty images measure nothing.
  CHECK_NEAR(measureGain(GreyImage(10, 10), GreyImage(20, 20), Box(0, 0, 5, 5)).face_gain, 0, 1e-9);
  CHECK_NEAR(measureGain(scene(12, 63, kFace), scene(2, 3, kFace), Box(5, 5, 5, 9)).face_gain, 0,
             1e-9);
  CHECK_NEAR(measureGain(GreyImage(), GreyImage(), kFace).ratio, 0, 1e-9);

  // A face filling the frame leaves no background: the ratio degenerates to the gain.
  const GainResult full = measureGain(scene(60, 60, Box(0, 0, 640, 400)),
                                      scene(2, 2, Box(0, 0, 640, 400)), Box(0, 0, 640, 400));
  CHECK_NEAR(full.bg_gain, 0.0, 1e-9);
  CHECK_NEAR(full.ratio, 58.0, 1e-4);
}

void testSpecular() {
  GreyImage lit(640, 400, 40);
  Landmarks lm;
  lm.valid = true;
  lm.x[Landmarks::kLeftEye] = 280;
  lm.y[Landmarks::kLeftEye] = 160;
  lm.x[Landmarks::kRightEye] = 360;
  lm.y[Landmarks::kRightEye] = 160;

  const std::vector<Box> eyes = eyeRegions(kFace, lm);
  CHECK(eyes.size() == 2);
  CHECK(countSpecular(lit, eyes) == 0);

  lit.at(280, 160) = 255;  // glint in the left eye
  lit.at(361, 159) = 250;  // right eye, exactly at the level
  lit.at(360, 161) = 249;  // just below it
  lit.at(300, 300) = 255;  // outside any eye
  CHECK(countSpecular(lit, eyes) == 2);

  // Without landmarks: the upper third of the face box (y 100..173).
  const std::vector<Box> upper = eyeRegions(kFace, Landmarks());
  CHECK(upper.size() == 1);
  CHECK(upper[0].y1 == 100 && upper[0].y2 == 173);
  CHECK(countSpecular(lit, upper) == 2);  // (280,160) and (361,159); 249 is below the level
}

}  // namespace

void testLiveness() {
  testRealFace();
  testScreenOrPhoto();
  testThresholds();
  testBackgroundIsRobust();
  testNegativeBackgroundAndEdgeCases();
  testSpecular();
}
