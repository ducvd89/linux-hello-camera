#include "core/config.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "core/file_util.h"

namespace lhc {

const char* const kDefaultConfigPath = LHC_CONFIG_PATH;
const char* const kFacesRoot = LHC_FACES_DIR;
const char* const kTemplatesDir = LHC_TEMPLATES_DIR;
const char* const kSocketPath = LHC_SOCKET_PATH;
const char* const kConfirmHook = LHC_CONFIRM_HOOK;

namespace {

// Assigns node[key] to `out` if present and convertible; otherwise leaves the
// default in place.
template <typename T>
void read(const YAML::Node& node, const char* key, T& out) {
  if (!node.IsDefined() || !node.IsMap() || !node[key]) {
    return;
  }
  try {
    out = node[key].as<T>();
  } catch (const YAML::Exception&) {
  }
}

void readStrings(const YAML::Node& node, const char* key, std::vector<std::string>& out) {
  if (!node.IsDefined() || !node.IsMap() || !node[key] || !node[key].IsSequence()) {
    return;
  }
  out.clear();
  for (const auto& item : node[key]) {
    if (item.IsScalar() && !item.Scalar().empty()) {
      out.push_back(item.Scalar());
    }
  }
}

}  // namespace

Config parseConfig(const std::string& yaml_text) {
  Config c;
  const YAML::Node root = YAML::Load(yaml_text);
  if (!root.IsMap()) {
    return c;
  }

  read(root, "schema_version", c.schema_version);
  read(root, "disabled", c.disabled);
  read(root, "camera", c.camera);
  read(root, "timeout_ms", c.timeout_ms);
  read(root, "abort_if_lid_closed", c.abort_if_lid_closed);
  read(root, "abort_if_ssh", c.abort_if_ssh);
  readStrings(root, "ignore_services", c.ignore_services);

  read(root["detection"], "model", c.detection.model);
  read(root["detection"], "threshold", c.detection.threshold);

  read(root["recognition"], "model", c.recognition.model);
  read(root["recognition"], "threshold", c.recognition.threshold);
  read(root["recognition"], "frames_needed", c.recognition.frames_needed);

  read(root["ir_liveness"], "enabled", c.ir_liveness.enabled);
  read(root["ir_liveness"], "min_pairs", c.ir_liveness.min_pairs);
  read(root["ir_liveness"], "min_face_gain", c.ir_liveness.min_face_gain);
  read(root["ir_liveness"], "min_gain_ratio", c.ir_liveness.min_gain_ratio);

  std::string tpm_setting;
  read(root["storage"], "tpm_encryption", tpm_setting);
  parseSetting(tpm_setting, c.storage.tpm_encryption);  // anything else stays auto

  read(root["ai_antispoof"], "enabled", c.ai_antispoof.enabled);
  read(root["ai_antispoof"], "model", c.ai_antispoof.model);
  read(root["ai_antispoof"], "threshold", c.ai_antispoof.threshold);

  read(root["confirm"], "enabled", c.confirm.enabled);
  readStrings(root["confirm"], "services", c.confirm.services);
  read(root["confirm"], "timeout_s", c.confirm.timeout_s);

  // A zero or negative value would make a match impossible or a timeout
  // instant; keep the numbers in a range where the flow still makes sense.
  c.timeout_ms = std::clamp(c.timeout_ms, 500, 60000);
  c.recognition.frames_needed = std::clamp(c.recognition.frames_needed, 1, 3);
  c.ir_liveness.min_pairs = std::clamp(c.ir_liveness.min_pairs, 0, 3);
  c.confirm.timeout_s = std::clamp(c.confirm.timeout_s, 1, 600);
  return c;
}

bool loadConfig(const std::string& path, Config& out, std::string& error) {
  std::ifstream file(path);
  if (!file) {
    error = "cannot read " + path;
    return false;
  }
  std::ostringstream text;
  text << file.rdbuf();
  try {
    out = parseConfig(text.str());
  } catch (const YAML::Exception& e) {
    error = "cannot parse " + path + ": " + e.what();
    return false;
  }
  return true;
}

bool writeTpmSetting(const std::string& path, TpmSetting setting, std::string& error) {
  YAML::Node root;
  std::string text;
  if (readFile(path, text)) {
    try {
      root = YAML::Load(text);
    } catch (const YAML::Exception& e) {
      error = "cannot parse " + path + ": " + e.what();
      return false;
    }
  }
  if (!root.IsMap()) {
    root = YAML::Node(YAML::NodeType::Map);
  }
  if (!root["storage"].IsMap()) {
    root["storage"] = YAML::Node(YAML::NodeType::Map);
  }
  root["storage"]["tpm_encryption"] = settingName(setting);

  YAML::Emitter out;
  out << root;
  return atomicWriteFile(path, std::string(out.c_str()) + "\n", 0644, error);
}

std::string modelsDir(bool allow_env) {
  if (allow_env) {
    const char* env = std::getenv(kModelsEnvVar);
    if (env != nullptr && env[0] != '\0') {
      return env;
    }
  }
  return LHC_MODELS_DIR;
}

std::string modelPath(const std::string& models_dir, const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) {
    return "";
  }
  return models_dir + "/" + name;
}

}  // namespace lhc
