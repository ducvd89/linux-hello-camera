#include <deque>
#include <memory>

#include "camera/camera_stream.h"
#include "camera/frame.h"
#include "tests/test.h"

using namespace lhc;

namespace {

void testUniform() {
  CHECK(isUniform(GreyImage(8, 4, 144)));
  CHECK(isUniform(GreyImage(8, 4, 0)));
  CHECK(!isUniform(GreyImage()));

  GreyImage almost(8, 4, 144);
  almost.at(7, 3) = 145;  // a single different pixel is a live frame
  CHECK(!isUniform(almost));
}

void testLitClassification() {
  // Observed on the target camera: unlit frames sit around 0.02-0.04, lit ones around 35-42.
  CHECK(!isLit(0.03));
  CHECK(!isLit(4.9));
  CHECK(isLit(5.0));
  CHECK(isLit(42.5));

  GreyImage dim(4, 4, 0);
  dim.at(0, 0) = 16;
  CHECK_NEAR(meanBrightness(dim), 1.0, 1e-9);
  CHECK(!isLit(meanBrightness(dim)));
  CHECK_NEAR(meanBrightness(GreyImage(4, 4, 40)), 40.0, 1e-9);
}

void testClassifyLitInDaylight() {
  // Observed in daylight: ambient IR lifts unlit frames to 27-50, all above kLitMeanMin, while
  // auto exposure ramps both up.
  const std::vector<double> means = {45.6, 26.7, 54.9, 35.4, 57.1, 37.1, 59.6, 38.7};
  const std::vector<bool> lit = classifyLit(means);
  for (size_t i = 0; i < means.size(); ++i) {
    CHECK(lit[i] == (i % 2 == 0));
  }

  // In the dark the strobe still splits as before.
  const std::vector<bool> dark = classifyLit({0.03, 40.0, 0.02, 38.0});
  CHECK(!dark[0] && dark[1] && !dark[2] && dark[3]);

  // A dropped frame (two unlit in a row) still classifies from the wider window.
  const std::vector<bool> dropped = classifyLit({30.0, 31.0, 60.0, 30.0, 61.0});
  CHECK(!dropped[0] && !dropped[1] && dropped[2] && !dropped[3] && dropped[4]);

  // No strobe: fall back to the absolute level.
  const std::vector<bool> steady = classifyLit({50.0, 51.0, 49.0});
  CHECK(steady[0] && steady[1] && steady[2]);
  const std::vector<bool> black = classifyLit({0.03, 0.04});
  CHECK(!black[0] && !black[1]);

  CHECK(classifyLit({}).empty());
  CHECK(classifyLit({42.0})[0]);
}

void testPairing() {
  // L U L U at 15 fps (~67 ms apart).
  std::vector<FrameStamp> strobe = {{0, true}, {67000, false}, {134000, true}, {201000, false}};
  CHECK(findUnlitPartner(strobe, 0) == 1);  // only the next frame exists
  CHECK(findUnlitPartner(strobe, 2) == 1);  // previous and next tie at 67 ms: the earlier wins

  // The previous frame is nearer than the next one.
  std::vector<FrameStamp> uneven = {{0, false}, {40000, true}, {140000, false}};
  CHECK(findUnlitPartner(uneven, 1) == 0);

  // A dropped frame leaves the partner 200 ms away: out of range.
  std::vector<FrameStamp> gap = {{0, true}, {200000, false}};
  CHECK(findUnlitPartner(gap, 0) == -1);
  CHECK(findUnlitPartner(gap, 0, 250000) == 1);

  // Lit neighbours are never partners.
  std::vector<FrameStamp> all_lit = {{0, true}, {67000, true}, {134000, true}};
  CHECK(findUnlitPartner(all_lit, 1) == -1);

  CHECK(findUnlitPartner(strobe, 9) == -1);
}

// Plays back a fixed list of frames, then times out.
class FakeSource : public FrameSource {
 public:
  explicit FakeSource(std::deque<GreyImage> frames) : frames_(std::move(frames)) {}

  ReadStatus read(Frame& out, int) override {
    if (frames_.empty()) {
      return ReadStatus::kTimeout;
    }
    out.image = std::move(frames_.front());
    frames_.pop_front();
    return ReadStatus::kOk;
  }

 private:
  std::deque<GreyImage> frames_;
};

void testReopenOnUniformFrames() {
  int opens = 0;
  CameraStream stream([&opens](std::string&) -> std::unique_ptr<FrameSource> {
    ++opens;
    std::deque<GreyImage> frames;
    if (opens == 1) {
      // A good frame, then the camera freezes on 144.
      frames.push_back(GreyImage(4, 4, 30));
      frames[0].at(1, 1) = 90;
      frames.push_back(GreyImage(4, 4, 144));
      frames.push_back(GreyImage(4, 4, 144));
    } else {
      frames.push_back(GreyImage(4, 4, 20));
      frames[0].at(2, 2) = 60;
    }
    return std::make_unique<FakeSource>(std::move(frames));
  });

  std::string error;
  CHECK(stream.open(error));
  Frame f;
  CHECK(stream.next(f, 100) == ReadStatus::kOk);
  CHECK(f.image.at(1, 1) == 90 && f.seq == 0);
  CHECK(opens == 1 && stream.reopenCount() == 0);

  // The flat frame is swallowed, the device is reopened, and the caller gets the next real frame.
  CHECK(stream.next(f, 100) == ReadStatus::kOk);
  CHECK(f.image.at(2, 2) == 60 && f.seq == 1);
  CHECK(opens == 2 && stream.reopenCount() == 1);

  CHECK(stream.next(f, 20) == ReadStatus::kTimeout);
}

void testOpenFailure() {
  CameraStream stream([](std::string& error) -> std::unique_ptr<FrameSource> {
    error = "busy";
    return nullptr;
  });
  std::string error;
  CHECK(!stream.open(error));
  CHECK(error == "busy");
  Frame f;
  CHECK(stream.next(f, 30) == ReadStatus::kError);
}

}  // namespace

void testCamera() {
  testUniform();
  testLitClassification();
  testClassifyLitInDaylight();
  testPairing();
  testReopenOnUniformFrames();
  testOpenFailure();
}
