#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lhc {

// TemplateEntry::kind. IR and colour pictures of a face never match each other.
inline constexpr const char* kIrKind = "ir";
inline constexpr const char* kColourKind = "colour";

struct TemplateEntry {
  std::string id;       // unix milliseconds of the capture
  int64_t created = 0;  // unix seconds
  std::vector<float> embedding;
  std::string face;     // the face (person, look) this picture belongs to: the id of its first entry
  std::string kind = kIrKind;  // which camera took it: kIrKind or kColourKind

};

// A user's face template: embeddings only, never images. Decrypted form of <user>.cred / the
// content of <user>.json (DESIGN.md "Face templates").
struct Template {
  int version = 1;
  std::string model;  // recognition model the embeddings came from
  int dim = 0;
  std::vector<TemplateEntry> entries;
  std::map<std::string, std::string> names;  // face id -> the name the user gave it
};

std::string templateToJson(const Template& tmpl);

// False (with `error`) if the text is not a version 1 template or an embedding does not have `dim`
// floats. Entries saved before faces existed have no "face"; they all join the face of the first
// such entry.
bool templateFromJson(const std::string& text, Template& out, std::string& error);

}  // namespace lhc
