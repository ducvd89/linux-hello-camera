#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lhc {

// Single-channel 8-bit image, row-major without padding. This is what the IR
// camera produces; the models get it replicated to three channels (see
// toRgb() in face/image_utils.h).
struct GreyImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> data;

  GreyImage() = default;
  GreyImage(int w, int h, uint8_t fill = 0)
      : width(w), height(h), data(static_cast<size_t>(w) * static_cast<size_t>(h), fill) {}

  bool empty() const { return data.empty(); }
  uint8_t at(int x, int y) const { return data[static_cast<size_t>(y) * width + x]; }
  uint8_t& at(int x, int y) { return data[static_cast<size_t>(y) * width + x]; }
};

}  // namespace lhc
