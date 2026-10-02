#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lhc {

struct TemplateEntry {
  std::string id;       // unix milliseconds of the capture
  int64_t created = 0;  // unix seconds
  std::vector<float> embedding;
};

// A user's face template: embeddings only, never images. Decrypted form of <user>.cred / the
// content of <user>.json (DESIGN.md "Face templates").
struct Template {
  int version = 1;
  std::string model;  // recognition model the embeddings came from
  int dim = 0;
  std::vector<TemplateEntry> entries;
};

std::string templateToJson(const Template& tmpl);

// False (with `error`) if the text is not a version 1 template or an embedding does not have `dim`
// floats.
bool templateFromJson(const std::string& text, Template& out, std::string& error);

}  // namespace lhc
