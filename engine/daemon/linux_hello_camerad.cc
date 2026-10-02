#include <CLI/CLI.hpp>
#include <string>

#include "camera/v4l2_camera.h"
#include "core/config.h"
#include "daemon/daemon_main.h"
#include "store/systemd_creds.h"

// The root daemon (DESIGN.md). Socket-activated by linux-hello-camerad.socket; `--socket PATH`
// binds a socket itself, for running it by hand.
int main(int argc, char** argv) {
  CLI::App app{"Linux Hello Camera daemon"};
  app.set_version_flag("--version,-v", LHC_VERSION);

  lhc::DaemonOptions options;
  options.engine.config_path = lhc::kDefaultConfigPath;
  options.engine.models_dir = lhc::modelsDir(false);
  options.engine.faces_dir = lhc::kFacesRoot;
  options.templates_dir = lhc::kTemplatesDir;
  app.add_option("--socket", options.socket_path, "Bind this socket instead of using systemd's");
  app.add_option("--idle-timeout", options.idle_timeout_s, "Exit after this many idle seconds");
  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  lhc::SystemdCreds crypto;
  return lhc::runDaemon(
      options, crypto,
      [](const std::string& device, std::string& error) -> std::unique_ptr<lhc::FrameSource> {
        return lhc::V4l2Camera::open(device, error);
      });
}
