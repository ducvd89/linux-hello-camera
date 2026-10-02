#include "face/enrolled_faces.h"

#include <dirent.h>

#include <algorithm>

namespace lhc {

namespace {

bool isPng(const std::string& name) {
  return name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0;
}

}  // namespace

std::vector<std::string> listFaces(const std::string& dir) {
  std::vector<std::string> faces;
  DIR* dp = opendir(dir.c_str());
  if (dp == nullptr) {
    return faces;
  }
  while (const dirent* entry = readdir(dp)) {
    const std::string name(entry->d_name);
    if (isPng(name)) {
      faces.push_back(dir + "/" + name);
    }
  }
  closedir(dp);
  std::sort(faces.begin(), faces.end());
  return faces;
}

bool writeGreyPng(const std::string& path, const ImageRGB& crop) {
  if (crop.empty()) {
    return false;
  }
  std::vector<uint8_t> grey(static_cast<size_t>(crop.width) * crop.height);
  for (size_t i = 0; i < grey.size(); i++) {
    grey[i] = crop.data[i * 3];  // the crop is grey replicated to three channels
  }
  return stbi_write_png(path.c_str(), crop.width, crop.height, 1, grey.data(), crop.width) != 0;
}

}  // namespace lhc
