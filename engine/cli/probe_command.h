#pragma once

#include <string>

namespace lhc {

// Developer/calibration tools: one JSON line per analysed lit frame (see DESIGN.md). They honour
// LHC_MODELS_DIR and --config, which `auth` never does.
struct ProbeOptions {
  std::string config_path;  // default: /etc/linux-hello-camera/config.yaml (missing = defaults)
  std::string faces_dir;    // score recognition against the PNG crops in this directory
  std::string save_crops;   // probe-dir: also write the crops enroll would pick to this directory
  std::string camera;       // probe: overrides config.camera
  double seconds = 5.0;     // probe: how long to capture
};

int runProbe(const ProbeOptions& options);
int runProbeDir(const std::string& dir, const ProbeOptions& options);

}  // namespace lhc
