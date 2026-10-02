#pragma once

#include <string>
#include <vector>

#include "core/hardware.h"

namespace lhc {

// Installed locations (DESIGN.md). They are compile-time settings (see CMakeLists.txt) so a
// packager or a test build can move them; nothing can change them at run time.
extern const char* const kDefaultConfigPath;  // /etc/linux-hello-camera/config.yaml
extern const char* const kFacesRoot;  // /var/lib/linux-hello-camera/faces (0.8 crops, migrate only)
extern const char* const kTemplatesDir;  // /var/lib/linux-hello-camera/templates
extern const char* const kSocketPath;    // /run/linux-hello-camera/engine.sock
extern const char* const kConfirmHook;   // /usr/lib/linux-hello-camera/confirm-hook

inline constexpr const char* kModelsEnvVar = "LHC_MODELS_DIR";

// Mirrors /etc/linux-hello-camera/config.yaml (see DESIGN.md). Every field has
// the default documented there, so a missing key or a bad value never turns
// into a surprise.
struct Config {
  int schema_version = 1;
  bool disabled = false;
  std::string camera;
  int timeout_ms = 4000;
  bool abort_if_lid_closed = true;
  bool abort_if_ssh = true;  // remote sessions never get face unlock
  std::vector<std::string> ignore_services;

  struct Detection {
    std::string model = "yolov8n-face.onnx";
    float threshold = 0.5f;
  } detection;

  struct Recognition {
    std::string model = "edgeface_s_gamma_05.onnx";
    float threshold = 0.5f;
    int frames_needed = 2;
  } recognition;

  struct IrLiveness {
    bool enabled = true;
    int min_pairs = 2;
    float min_face_gain = 25.0f;
    float min_gain_ratio = 1.8f;
  } ir_liveness;

  struct Storage {
    TpmSetting tpm_encryption = TpmSetting::kAuto;
  } storage;

  struct AiAntispoof {
    bool enabled = false;
    std::string model = "minifas_v2.onnx";
    float threshold = 0.8f;
  } ai_antispoof;

  struct Confirm {
    bool enabled = true;
    std::vector<std::string> services = {"sudo", "polkit-1"};
    int timeout_s = 15;
  } confirm;
};

// Parses config.yaml text. Unknown keys are ignored, missing or mistyped keys
// keep their default, out-of-range numbers are clamped. Throws
// YAML::ParserException only when the text is not YAML at all.
Config parseConfig(const std::string& yaml_text);

// Reads and parses `path`. Returns false (with `error` set) when the file is
// missing or unreadable or is not valid YAML.
bool loadConfig(const std::string& path, Config& out, std::string& error);

// Sets storage.tpm_encryption in the config file at `path` and leaves everything else in it
// as it was (as yaml-cpp re-emits it), writing atomically with mode 0644. Creates the file if
// it does not exist.
bool writeTpmSetting(const std::string& path, TpmSetting setting, std::string& error);

// The models directory: the compiled-in default, or $LHC_MODELS_DIR when
// `allow_env` is set. Only dev commands (probe, probe-dir) pass true; `auth`
// runs as root from PAM and must not be steerable by its environment.
std::string modelsDir(bool allow_env);

// models_dir + "/" + name; empty if `name` is not a plain file name.
std::string modelPath(const std::string& models_dir, const std::string& name);

}  // namespace lhc
