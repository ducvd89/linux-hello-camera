#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lhc {

std::string base64Encode(const uint8_t* data, size_t size);

// Strict standard base64 with padding; false on any other character or a bad length.
bool base64Decode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace lhc
