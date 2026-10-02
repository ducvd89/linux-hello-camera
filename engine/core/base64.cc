#include "core/base64.h"

namespace lhc {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int valueOf(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

}  // namespace

std::string base64Encode(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = i + 1 < size ? data[i + 1] : 0;
    const uint32_t b2 = i + 2 < size ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out += kAlphabet[(triple >> 18) & 63];
    out += kAlphabet[(triple >> 12) & 63];
    out += i + 1 < size ? kAlphabet[(triple >> 6) & 63] : '=';
    out += i + 2 < size ? kAlphabet[triple & 63] : '=';
  }
  return out;
}

bool base64Decode(const std::string& text, std::vector<uint8_t>& out) {
  out.clear();
  if (text.size() % 4 != 0) {
    return false;
  }
  for (size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int k = 0; k < 4; k++) {
      const char c = text[i + k];
      if (c == '=') {
        // Padding only in the last two positions of the last group.
        if (i + 4 != text.size() || k < 2)
          return false;
        v[k] = 0;
        pad++;
      } else {
        if (pad > 0)
          return false;
        v[k] = valueOf(c);
        if (v[k] < 0)
          return false;
      }
    }
    const uint32_t triple = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
    out.push_back((triple >> 16) & 0xFF);
    if (pad < 2)
      out.push_back((triple >> 8) & 0xFF);
    if (pad < 1)
      out.push_back(triple & 0xFF);
  }
  return true;
}

}  // namespace lhc
