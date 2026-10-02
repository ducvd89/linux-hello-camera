#include "analysis/crop_selector.h"
#include "analysis/match_tracker.h"
#include "face/recognition/face_recognition.h"
#include "tests/test.h"

using namespace lhc;

namespace {

void testCosine() {
  CHECK_NEAR(cosineSimilarity({1, 0, 0}, {1, 0, 0}), 1.0, 1e-6);
  CHECK_NEAR(cosineSimilarity({1, 0}, {0, 1}), 0.0, 1e-6);
  CHECK_NEAR(cosineSimilarity({1, 2, 3}, {-1, -2, -3}), -1.0, 1e-6);
  CHECK_NEAR(cosineSimilarity({1, 1}, {1, 0}), std::sqrt(0.5), 1e-6);
  CHECK_NEAR(cosineSimilarity({2, 0}, {10, 0}), 1.0, 1e-6);  // magnitude does not matter
  CHECK_NEAR(cosineSimilarity({0, 0}, {1, 1}), 0.0, 1e-9);   // no magnitude: no similarity
  CHECK_NEAR(cosineSimilarity({1, 1}, {1, 1, 1}), 0.0, 1e-9);
  CHECK_NEAR(cosineSimilarity({}, {}), 0.0, 1e-9);
}

void testTracker() {
  // frames_needed 2, liveness off: two good frames among the last three.
  MatchTracker plain(2, 0);
  plain.add(true);
  CHECK(!plain.satisfied());
  plain.add(false);
  plain.add(true);
  CHECK(plain.satisfied());
  plain.add(false);  // window is now F T F: only one good frame
  CHECK(!plain.satisfied());
  plain.add(true);  // T F T
  CHECK(plain.satisfied());

  // Frames outside the window of three do not count.
  MatchTracker window(2, 0);
  window.add(true);
  window.add(false);
  window.add(false);
  window.add(true);
  CHECK(!window.satisfied());

  // min_pairs raises the bar when liveness is on, and the window grows to fit.
  MatchTracker strict(2, 4);
  for (int i = 0; i < 3; ++i) {
    strict.add(true);
  }
  CHECK(!strict.satisfied());
  strict.add(true);
  CHECK(strict.satisfied());

  MatchTracker single(1, 0);
  single.add(true);
  CHECK(single.satisfied());
}

Observation goodObs(int64_t t_us, std::vector<float> embedding) {
  Observation obs;
  obs.face_found = true;
  obs.det_score = 0.9f;
  obs.timestamp_us = t_us;
  obs.embedding = std::move(embedding);
  obs.paired = true;
  obs.gain.face_gain = 60;
  obs.gain.ratio = 6;
  return obs;
}

void testCropSelector() {
  CropSelector sel{CropSelectorParams()};
  CHECK(sel.accept(goodObs(0, {1, 0, 0})));
  CHECK(!sel.accept(goodObs(100000, {0, 1, 0})));     // too soon after the last crop
  CHECK(!sel.accept(goodObs(500000, {1, 0.1f, 0})));  // near-duplicate (cosine ~0.995)
  CHECK(sel.accept(goodObs(500000, {0, 1, 0})));
  CHECK(sel.accept(goodObs(900000, {0, 0, 1})));

  Observation weak = goodObs(2000000, {0.5f, 0.5f, 0.5f});
  weak.det_score = 0.6f;
  CHECK(!sel.accept(weak));

  Observation no_pair = goodObs(2000000, {0.5f, 0.5f, 0.5f});
  no_pair.paired = false;
  CHECK(!sel.accept(no_pair));
  Observation flat = goodObs(2000000, {0.5f, 0.5f, 0.5f});
  flat.gain.face_gain = 5;  // a photo or screen: no strobe response
  CHECK(!sel.accept(flat));
  CHECK(sel.accept(goodObs(2000000, {0.5f, 0.5f, 0.5f})));

  CropSelectorParams relaxed;
  relaxed.require_liveness = false;
  CropSelector no_live(relaxed);
  CHECK(no_live.accept(flat));

  Observation no_face;
  CHECK(!no_live.accept(no_face));
}

}  // namespace

void testMatching() {
  testCosine();
  testTracker();
  testCropSelector();
}
