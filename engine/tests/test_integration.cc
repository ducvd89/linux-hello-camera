// enroll -> list -> auth -> remove -> clear through the helper's client code, against the test
// daemon (fake TPM) with the real models, replaying recorded IR sessions as the camera.
#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "cli/auth_command.h"
#include "cli/daemon_client.h"
#include "tests/temp_dir.h"
#include "tests/test.h"

using namespace lhc;
using lhc_test::TempDir;
using nlohmann::json;

namespace {

constexpr int kSkip = 77;

std::vector<std::string> recordedSessions() {
  const char* home = std::getenv("HOME");
  std::vector<std::string> sessions;
  if (home == nullptr)
    return sessions;
  const std::string root = std::string(home) + "/.local/share/linux-hello-camera/recordings";
  DIR* dir = opendir(root.c_str());
  if (dir == nullptr)
    return sessions;
  while (const dirent* e = readdir(dir)) {
    const std::string path = root + "/" + e->d_name;
    struct stat st;
    if (e->d_name[0] != '.' && stat((path + "/session.json").c_str(), &st) == 0) {
      sessions.push_back(path);
    }
  }
  closedir(dir);
  std::sort(sessions.begin(), sessions.end());
  return sessions;
}

std::string configText(const std::string& camera, const std::string& extra = "") {
  return "schema_version: 1\ncamera: " + camera +
         "\ntimeout_ms: 4000\nabort_if_lid_closed: false\nabort_if_ssh: false\n"
         "confirm:\n  enabled: false\nstorage:\n  tpm_encryption: on\n" +
         extra;
}

pid_t startDaemon(const TempDir& dir) {
  const pid_t pid = fork();
  if (pid == 0) {
    execl(TEST_DAEMON, "lhc_test_daemon", "--socket", dir.file("run/engine.sock").c_str(),
          "--config", dir.file("config.yaml").c_str(), "--templates", dir.file("templates").c_str(),
          "--models", TEST_MODELS_DIR, "--faces", dir.file("faces").c_str(), "--hardware-root",
          dir.file("hw").c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return pid;
}

CallResult ask(const ClientContext& context, const std::string& line, int timeout_ms = 60000,
               const std::function<void(const json&)>& progress = nullptr) {
  Request r;
  std::string error;
  CHECK(parseRequest(line, r, error));
  return callDaemon(context, r, timeout_ms, progress);
}

// No picture of the user may be anywhere under `root`: not as a file extension, not as bytes.
bool hasImages(const std::string& root, const std::string& except_dir) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().string().rfind(except_dir, 0) == 0)
      continue;
    std::string content;
    lhc_test::readFileForTest(entry.path().string(), content);
    if (content.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 || content.compare(0, 2, "\xff\xd8") == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const std::vector<std::string> sessions = recordedSessions();
  struct stat st;
  if (sessions.size() < 2 ||
      stat((std::string(TEST_MODELS_DIR) + "/yolov8n-face.onnx").c_str(), &st) != 0) {
    std::printf("skip: needs two recorded sessions and the models in %s\n", TEST_MODELS_DIR);
    return kSkip;
  }
  const std::string enroll_session = sessions.back();  // enrol from one recording...
  const std::string auth_session = sessions.front();   // ...and authenticate with another

  TempDir dir;
  lhc_test::fakeHardware(dir, true, true, "hw/");  // a PC with TPM 2.0 and Secure Boot
  dir.write("config.yaml", configText(enroll_session));
  const pid_t daemon = startDaemon(dir);

  ClientContext context;
  context.socket_path = dir.file("run/engine.sock");
  context.config_path = dir.file("config.yaml");

  // Wait for the daemon to listen (it loads nothing until the first camera request).
  bool up = false;
  for (int i = 0; i < 100 && !up; i++) {
    up = ask(context, R"({"cmd":"status"})", 2000).status == CallStatus::kOk;
    if (!up)
      usleep(100000);
  }
  CHECK(up);
  if (!up) {
    kill(daemon, SIGKILL);
    waitpid(daemon, nullptr, 0);
    return 1;
  }

  const std::string user = "tester";
  const std::string user_json = R"("username":"tester")";

  CallResult r = ask(context, R"({"cmd":"status"})");
  CHECK(r.reply["effective"] == "tpm-sb");
  r = ask(context, R"({"cmd":"list",)" + user_json + "}");
  CHECK(r.reply["result"] == "not_enrolled");

  // enroll: progress lines, then ok. Only embeddings are stored.
  std::vector<int> progress;
  r = ask(context, R"({"cmd":"enroll",)" + user_json + R"(,"count":3})", 120000,
          [&](const json& p) { progress.push_back(p["progress"].get<int>()); });
  CHECK(r.status == CallStatus::kOk && r.reply["result"] == "ok" && r.reply["added"] == 3);
  CHECK((progress == std::vector<int>{1, 2, 3}));
  CHECK(!hasImages(dir.path(), dir.file("hw")));
  CHECK(std::filesystem::exists(dir.file("templates/tester.tpm-sb.cred")));

  r = ask(context, R"({"cmd":"list",)" + user_json + "}");
  CHECK(r.reply["result"] == "ok" && r.reply["entries"].size() == 3 &&
        r.reply["encryption"] == "tpm-sb");
  const std::string first_id = r.reply["entries"][0]["id"].get<std::string>();
  CHECK(r.reply.dump().find("embedding") == std::string::npos);

  // auth through the helper's own auth command, with another recording as the camera.
  dir.write("config.yaml", configText(auth_session));
  CHECK(runAuthCommand(context, user, "sudo", false) == kExitMatch);
  // Someone not enrolled: ignore, so PAM goes on to the password.
  CHECK(runAuthCommand(context, "stranger", "sudo", false) == kExitIgnore);

  // `test` reports the details; a strict threshold or liveness setting makes the same face fail.
  r = ask(context, R"({"cmd":"test",)" + user_json + "}");
  CHECK(r.reply["result"] == "ok" && r.reply["best_score"].get<double>() > 0.5 &&
        r.reply["liveness_pairs"].get<int>() >= 2);
  dir.write("config.yaml", configText(auth_session, "recognition:\n  threshold: 0.999\n"));
  r = ask(context, R"({"cmd":"test",)" + user_json + "}");
  CHECK(r.reply["result"] == "not_recognised");
  CHECK(runAuthCommand(context, user, "sudo", false) == kExitNoMatch);
  dir.write("config.yaml", configText(auth_session, "ir_liveness:\n  min_face_gain: 500\n"));
  r = ask(context, R"({"cmd":"test",)" + user_json + "}");
  CHECK(r.reply["result"] == "liveness_failed");

  // Other settings: disabled and a recording that has no frames to match.
  dir.write("config.yaml", configText(auth_session, "disabled: true\n"));
  CHECK(runAuthCommand(context, user, "sudo", false) == kExitIgnore);

  // Turning encryption off converts the template in place.
  dir.write("config.yaml", configText(auth_session));
  r = ask(context, R"({"cmd":"set-encryption","value":"off"})");
  CHECK(r.reply["result"] == "ok" && r.reply["effective"] == "none" && r.reply["converted"] == 1);
  CHECK(std::filesystem::exists(dir.file("templates/tester.json")));
  CHECK(!std::filesystem::exists(dir.file("templates/tester.tpm-sb.cred")));
  r = ask(context, R"({"cmd":"test",)" + user_json + "}");
  CHECK(r.reply["result"] == "ok");  // still matches after the conversion

  // remove one entry, then clear.
  r = ask(context, R"({"cmd":"remove",)" + user_json + R"(,"id":")" + first_id + R"("})");
  CHECK(r.reply["result"] == "ok");
  r = ask(context, R"({"cmd":"list",)" + user_json + "}");
  CHECK(r.reply["entries"].size() == 2);
  r = ask(context, R"({"cmd":"clear",)" + user_json + "}");
  CHECK(r.reply["result"] == "ok");
  r = ask(context, R"({"cmd":"list",)" + user_json + "}");
  CHECK(r.reply["result"] == "not_enrolled");
  CHECK(runAuthCommand(context, user, "sudo", false) == kExitIgnore);

  // migrate: 0.8 crops become a template and are deleted. Whole frames stand in for crops here.
  std::filesystem::create_directories(dir.file("faces/tester"));
  std::filesystem::copy_file(enroll_session + "/000010.png",
                             dir.file("faces/tester/1790000000100.png"));
  std::filesystem::copy_file(enroll_session + "/000030.png",
                             dir.file("faces/tester/1790000000200.png"));
  r = ask(context, R"({"cmd":"migrate"})");
  CHECK(r.reply["result"] == "ok" && r.reply["migrated"] == json::array({"tester"}));
  CHECK(!std::filesystem::exists(dir.file("faces")));
  r = ask(context, R"({"cmd":"list",)" + user_json + "}");
  CHECK(r.reply["result"] == "ok" && r.reply["entries"].size() == 2);
  CHECK(!hasImages(dir.path(), dir.file("hw")));

  kill(daemon, SIGTERM);
  int status = 0;
  waitpid(daemon, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(!std::filesystem::exists(dir.file("run/engine.sock")));

  const auto& result = lhc_test::registry();
  std::printf("%d checks, %d failures\n", result.checks, result.failures);
  return result.failures == 0 ? 0 : 1;
}
