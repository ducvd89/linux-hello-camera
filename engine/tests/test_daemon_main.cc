// A daemon for tests and hand checks (never installed): the real engine and server, with a fake
// TPM instead of systemd-creds and, when the configured "camera" is a recorded session folder, a
// replay of its frames instead of V4L2. Runs as an ordinary user, who counts as root for it.
#include <sys/stat.h>
#include <unistd.h>

#include <CLI/CLI.hpp>

#include "camera/replay_source.h"
#include "camera/v4l2_camera.h"
#include "daemon/daemon_main.h"
#include "tests/fake_crypto.h"

int main(int argc, char** argv) {
  CLI::App app{"Linux Hello Camera test daemon"};
  lhc::DaemonOptions options;
  options.log_stderr = true;
  options.root_uid = getuid();
  options.idle_timeout_s = 0;
  app.add_option("--socket", options.socket_path)->required();
  app.add_option("--config", options.engine.config_path)->required();
  app.add_option("--templates", options.templates_dir)->required();
  app.add_option("--models", options.engine.models_dir)->required();
  app.add_option("--faces", options.engine.faces_dir);
  app.add_option("--hardware-root", options.engine.hardware_root);
  app.add_option("--idle-timeout", options.idle_timeout_s);
  CLI11_PARSE(app, argc, argv);

  lhc_test::FakeCrypto crypto;
  return lhc::runDaemon(
      options, crypto,
      [](const std::string& device, std::string& error) -> std::unique_ptr<lhc::FrameSource> {
        struct stat st;
        if (stat(device.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
          return lhc::ReplaySource::open(device, error);
        }
        return lhc::V4l2Camera::open(device, error);
      });
}
