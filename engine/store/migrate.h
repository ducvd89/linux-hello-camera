#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "face/image_utils.h"
#include "store/template_store.h"

namespace lhc {

struct MigrateResult {
  std::vector<std::string> migrated;                        // users whose crops are now a template
  std::vector<std::pair<std::string, std::string>> failed;  // user, why their crops were kept
};

using Embedder = std::function<std::vector<float>(const ImageRGB&)>;

// Upgrade from 0.8: embeds every PNG crop in <faces_root>/<user>/ with `embed`, merges the
// embeddings into the user's template (written as `target`), then overwrites the crops with zeros,
// deletes them and removes the then empty directories. Safe to run again: entries already in the
// template (the crop's file name is its id) are not added twice. A user whose crops can't be
// embedded keeps them and is reported in `failed`.
MigrateResult migrateFaces(const std::string& faces_root, TemplateStore& store,
                           const std::string& model, Storage target, const Embedder& embed);

// Overwrites the file with zeros, fsyncs and unlinks it.
bool shredFile(const std::string& path);

}  // namespace lhc
