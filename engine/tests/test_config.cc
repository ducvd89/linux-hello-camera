#include <yaml-cpp/yaml.h>

#include <cstdlib>

#include "core/config.h"
#include "tests/test.h"

using namespace lhc;

namespace {

void testDefaults() {
  const Config c = parseConfig("");
  CHECK(!c.disabled);
  CHECK(c.camera.empty());
  CHECK(c.timeout_ms == 4000);
  CHECK(c.abort_if_lid_closed);
  CHECK(c.abort_if_ssh);
  CHECK(c.ignore_services.empty());
  CHECK(c.detection.model == "yolov8n-face.onnx");
  CHECK_NEAR(c.detection.threshold, 0.5, 1e-6);
  CHECK(c.recognition.model == "edgeface_s_gamma_05.onnx");
  CHECK_NEAR(c.recognition.threshold, 0.5, 1e-6);
  CHECK(c.recognition.frames_needed == 2);
  CHECK(c.ir_liveness.enabled);
  CHECK(c.ir_liveness.min_pairs == 2);
  CHECK_NEAR(c.ir_liveness.min_face_gain, 25.0, 1e-6);
  CHECK_NEAR(c.ir_liveness.min_gain_ratio, 1.8, 1e-6);
  CHECK(!c.ai_antispoof.enabled);
  CHECK(c.ai_antispoof.model == "minifas_v2.onnx");
  CHECK_NEAR(c.ai_antispoof.threshold, 0.8, 1e-6);
  CHECK(c.confirm.enabled);
  CHECK((c.confirm.services == std::vector<std::string>{"sudo", "polkit-1"}));
}

void testOverridesAndUnknownKeys() {
  const Config c = parseConfig(R"(
schema_version: 1
disabled: true
abort_if_ssh: false
camera: /dev/video2
timeout_ms: 6000
future_option: whatever
ignore_services: [gdm-password, login]
recognition:
  threshold: 0.62
  new_nested_key: 1
ir_liveness:
  enabled: false
  min_face_gain: 31.5
confirm:
  services: [sudo]
unknown_section:
  a: [1, 2, 3]
)");
  CHECK(c.disabled);
  CHECK(!c.abort_if_ssh);
  CHECK(c.camera == "/dev/video2");
  CHECK(c.timeout_ms == 6000);
  CHECK((c.ignore_services == std::vector<std::string>{"gdm-password", "login"}));
  CHECK_NEAR(c.recognition.threshold, 0.62, 1e-6);
  CHECK(c.recognition.frames_needed == 2);  // untouched keys keep their defaults
  CHECK(!c.ir_liveness.enabled);
  CHECK_NEAR(c.ir_liveness.min_face_gain, 31.5, 1e-6);
  CHECK_NEAR(c.ir_liveness.min_gain_ratio, 1.8, 1e-6);
  CHECK((c.confirm.services == std::vector<std::string>{"sudo"}));
  CHECK(c.confirm.enabled);
}

void testBadValues() {
  // Wrong types fall back to the default instead of failing the whole file.
  const Config c =
      parseConfig("timeout_ms: soon\nrecognition:\n  threshold: [1]\ndisabled: true\n");
  CHECK(c.timeout_ms == 4000);
  CHECK_NEAR(c.recognition.threshold, 0.5, 1e-6);
  CHECK(c.disabled);

  // Out-of-range numbers are clamped.
  const Config d = parseConfig(
      "timeout_ms: 0\nrecognition:\n  frames_needed: 0\nir_liveness:\n  min_pairs: 99\n");
  CHECK(d.timeout_ms == 500);
  CHECK(d.recognition.frames_needed == 1);
  CHECK(d.ir_liveness.min_pairs == 3);

  // A root that is not a map is just defaults; text that is not YAML at all throws.
  CHECK(parseConfig("- a\n- b\n").timeout_ms == 4000);
  bool threw = false;
  try {
    parseConfig("camera: [unclosed\n");
  } catch (const YAML::Exception&) {
    threw = true;
  }
  CHECK(threw);
}

void testModelsDirEnvironment() {
  // Only the dev commands may redirect the models; auth never looks at the environment.
  setenv(kModelsEnvVar, "/tmp/other-models", 1);
  CHECK(modelsDir(true) == "/tmp/other-models");
  CHECK(modelsDir(false) != "/tmp/other-models");
  setenv(kModelsEnvVar, "", 1);
  CHECK(modelsDir(true) == modelsDir(false));
  unsetenv(kModelsEnvVar);
}

void testModelPaths() {
  CHECK(modelPath("/m", "yolo.onnx") == "/m/yolo.onnx");
  CHECK(modelPath("/m", "../etc/passwd").empty());
  CHECK(modelPath("/m", "").empty());
  CHECK(modelPath("/m", "..").empty());
}

}  // namespace

void testConfig() {
  testDefaults();
  testOverridesAndUnknownKeys();
  testBadValues();
  testModelsDirEnvironment();
  testModelPaths();
}
