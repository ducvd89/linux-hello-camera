#include "store/template.h"

#include <cstring>
#include <nlohmann/json.hpp>

#include "core/base64.h"

namespace lhc {

namespace {

// Embeddings travel as little-endian float32 whatever the host is.
std::string encodeEmbedding(const std::vector<float>& values) {
  std::vector<uint8_t> bytes;
  bytes.reserve(values.size() * 4);
  for (float v : values) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int shift = 0; shift < 32; shift += 8) {
      bytes.push_back((bits >> shift) & 0xFF);
    }
  }
  return base64Encode(bytes.data(), bytes.size());
}

bool decodeEmbedding(const std::string& text, std::vector<float>& out) {
  std::vector<uint8_t> bytes;
  if (!base64Decode(text, bytes) || bytes.size() % 4 != 0) {
    return false;
  }
  out.clear();
  for (size_t i = 0; i < bytes.size(); i += 4) {
    const uint32_t bits = bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16) |
                          (static_cast<uint32_t>(bytes[i + 3]) << 24);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    out.push_back(v);
  }
  return true;
}

}  // namespace

std::string templateToJson(const Template& tmpl) {
  nlohmann::json entries = nlohmann::json::array();
  for (const TemplateEntry& e : tmpl.entries) {
    entries.push_back({{"id", e.id},
                       {"created", e.created},
                       {"face", e.face},
                       {"embedding", encodeEmbedding(e.embedding)}});
  }
  nlohmann::json doc = {{"version", tmpl.version},
                        {"model", tmpl.model},
                        {"dim", tmpl.dim},
                        {"entries", entries},
                        {"names", tmpl.names}};
  return doc.dump();
}

bool templateFromJson(const std::string& text, Template& out, std::string& error) {
  try {
    const nlohmann::json doc = nlohmann::json::parse(text);
    Template t;
    t.version = doc.at("version").get<int>();
    if (t.version != 1) {
      error = "unsupported template version";
      return false;
    }
    t.model = doc.at("model").get<std::string>();
    t.dim = doc.at("dim").get<int>();
    std::string legacy_face;
    for (const auto& item : doc.at("entries")) {
      TemplateEntry e;
      e.id = item.at("id").get<std::string>();
      e.created = item.at("created").get<int64_t>();
      e.face = item.value("face", std::string());
      if (e.face.empty()) {
        if (legacy_face.empty()) {
          legacy_face = e.id;
        }
        e.face = legacy_face;
      }
      if (!decodeEmbedding(item.at("embedding").get<std::string>(), e.embedding) ||
          static_cast<int>(e.embedding.size()) != t.dim) {
        error = "bad embedding in template";
        return false;
      }
      t.entries.push_back(std::move(e));
    }
    if (doc.contains("names")) {
      t.names = doc.at("names").get<std::map<std::string, std::string>>();
    }
    out = std::move(t);
    return true;
  } catch (const nlohmann::json::exception& e) {
    error = std::string("bad template: ") + e.what();
    return false;
  }
}

}  // namespace lhc
