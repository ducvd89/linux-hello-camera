#pragma once

#include <string>
#include <vector>

#include "face/image_utils.h"

namespace lhc {

// PNG crops (*.png) in `dir`, sorted by name; empty if the directory is unreadable. Crops are no
// longer stored (templates hold embeddings only); this serves the 0.8 migration and the dev tools.
std::vector<std::string> listFaces(const std::string& dir);

// Writes a crop (grey replicated to three channels) as a grey PNG; dev tooling only.
bool writeGreyPng(const std::string& path, const ImageRGB& crop);

}  // namespace lhc
