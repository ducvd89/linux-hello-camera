#include "camera/replay_source.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "face/image_utils.h"

namespace lhc {

std::unique_ptr<ReplaySource> ReplaySource::open(const std::string& dir, std::string& error) {
  std::unique_ptr<ReplaySource> source(new ReplaySource());
  try {
    // session.json is plain JSON, which yaml-cpp reads.
    const YAML::Node session = YAML::LoadFile(dir + "/session.json");
    const YAML::Node frames = session["frames"];
    if (!frames.IsSequence()) {
      error = dir + "/session.json has no frames";
      return nullptr;
    }
    uint32_t index = 0;
    for (const YAML::Node& entry : frames) {
      source->items_.push_back({dir + "/" + entry["file"].as<std::string>(),
                                std::llround(entry["t"].as<double>() * 1e6),
                                entry["seq"].as<uint32_t>(index)});
      index++;
    }
  } catch (const YAML::Exception& e) {
    error = "cannot read " + dir + "/session.json: " + e.what();
    return nullptr;
  }
  return source;
}

ReadStatus ReplaySource::read(Frame& out, int timeout_ms) {
  while (next_ < items_.size()) {
    const Item& item = items_[next_++];
    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load(item.path.c_str(), &w, &h, &channels, 1);
    if (pixels == nullptr) {
      spdlog::warn("cannot read {}", item.path);
      continue;
    }
    out.image = GreyImage(w, h);
    std::copy(pixels, pixels + out.image.data.size(), out.image.data.begin());
    stbi_image_free(pixels);
    out.timestamp_us = item.timestamp_us;
    out.seq = item.seq;
    return ReadStatus::kOk;
  }
  // Nothing left: behave like a camera that went quiet, without letting callers spin.
  std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 20)));
  return ReadStatus::kTimeout;
}

}  // namespace lhc
