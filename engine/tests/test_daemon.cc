#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "cli/daemon_client.h"
#include "daemon/authorization.h"
#include "daemon/engine.h"
#include "daemon/protocol.h"
#include "daemon/rate_limiter.h"
#include "daemon/server.h"
#include "tests/fake_crypto.h"
#include "tests/temp_dir.h"
#include "tests/test.h"

using namespace lhc;
using lhc_test::FakeCrypto;
using lhc_test::TempDir;
using nlohmann::json;

namespace {

Request parseOk(const std::string& line) {
  Request r;
  std::string error;
  CHECK(parseRequest(line, r, error));
  return r;
}

bool parseFails(const std::string& line, const std::string& expected_error = "") {
  Request r;
  std::string error;
  const bool ok = parseRequest(line, r, error);
  return !ok && (expected_error.empty() || error == expected_error);
}

void testProtocol() {
  Request r = parseOk(R"({"cmd":"auth","username":"alice","service":"sudo","remote":true})");
  CHECK(r.cmd == Cmd::kAuth && r.username == "alice" && r.service == "sudo" && r.remote);
  r = parseOk(R"({"cmd":"auth","username":"alice"})");
  CHECK(r.service.empty() && !r.remote);
  r = parseOk(R"({"cmd":"enroll","username":"alice","count":3})");
  CHECK(r.cmd == Cmd::kEnroll && r.count == 3);
  r = parseOk(R"({"cmd":"enroll","username":"alice"})");
  CHECK(r.count == 5);
  r = parseOk(R"({"cmd":"remove","username":"alice","id":"1790000000100"})");
  CHECK(r.cmd == Cmd::kRemove && r.id == "1790000000100");
  r = parseOk(R"({"cmd":"remove","username":"alice","face":"1790000000100"})");
  CHECK(r.cmd == Cmd::kRemove && r.id.empty() && r.face == "1790000000100");
  r = parseOk(R"({"cmd":"enroll","username":"alice","name":"Đức với kính"})");
  CHECK(r.face.empty() && r.name == "Đức với kính");
  r = parseOk(R"({"cmd":"enroll","username":"alice","face":"1000"})");
  CHECK(r.face == "1000" && r.name.empty());
  r = parseOk(R"({"cmd":"rename","username":"alice","face":"1000","name":"Me"})");
  CHECK(r.cmd == Cmd::kRename && r.face == "1000" && r.name == "Me");
  CHECK(parseOk(R"({"cmd":"list","username":"alice"})").cmd == Cmd::kList);
  CHECK(parseOk(R"({"cmd":"clear","username":"alice"})").cmd == Cmd::kClear);
  CHECK(parseOk(R"({"cmd":"test","username":"alice"})").cmd == Cmd::kTest);
  CHECK(parseOk(R"({"cmd":"migrate"})").cmd == Cmd::kMigrate);
  CHECK(parseOk(R"({"cmd":"status"})").cmd == Cmd::kStatus);
  r = parseOk(R"({"cmd":"set-encryption","value":"auto"})");
  CHECK(r.cmd == Cmd::kSetEncryption && r.value == "auto");
  CHECK(parseOk(R"({"cmd":"auth","username":"alice","extra":1})").username ==
        "alice");  // unknown keys ignored

  CHECK(parseFails("", "malformed request"));
  CHECK(parseFails("not json", "malformed request"));
  CHECK(parseFails("[1,2]", "malformed request"));
  CHECK(parseFails(R"({"username":"a"})", "malformed request"));
  CHECK(parseFails(R"({"cmd":5})", "malformed request"));
  CHECK(parseFails(R"({"cmd":"reboot"})", "unknown command"));
  CHECK(parseFails(R"({"cmd":"auth"})", "bad username"));
  CHECK(parseFails(R"({"cmd":"auth","username":"../etc"})", "bad username"));
  CHECK(parseFails(R"({"cmd":"auth","username":""})", "bad username"));
  CHECK(parseFails(R"({"cmd":"auth","username":7})", "malformed request"));
  CHECK(parseFails(R"({"cmd":"auth","username":"a","remote":"yes"})", "malformed request"));
  CHECK(parseFails(R"({"cmd":"enroll","username":"a","count":0})", "bad count"));
  CHECK(parseFails(R"({"cmd":"enroll","username":"a","count":21})", "bad count"));
  CHECK(parseFails(R"({"cmd":"enroll","username":"a","count":"5"})", "malformed request"));
  CHECK(parseFails(R"({"cmd":"remove","username":"a"})", "missing id"));
  CHECK(parseFails(R"({"cmd":"remove","username":"a","id":"1","face":"1"})",
                   "id and face both given"));
  CHECK(parseFails(R"({"cmd":"enroll","username":"a","face":"1","name":"x"})", "bad name"));
  CHECK(parseFails(R"({"cmd":"enroll","username":"a","name":"a\nb"})", "bad name"));
  CHECK(parseFails(R"({"cmd":"rename","username":"a","name":"x"})", "missing face"));
  CHECK(parseFails(R"({"cmd":"rename","username":"a","face":"1"})", "bad name"));
  CHECK(parseFails(R"({"cmd":"rename","username":"a","face":"1","name":")" +
                       std::string(129, 'x') + R"("})",
                   "bad name"));
  CHECK(parseFails(R"({"cmd":"rename","username":"a","face":"1","name":5})", "malformed request"));
  CHECK(parseFails(R"({"cmd":"set-encryption","value":"maybe"})", "bad value"));
  CHECK(parseFails(R"({"cmd":"set-encryption"})", "bad value"));
  CHECK(parseFails(std::string(5000, ' ') + R"({"cmd":"status"})", "request too long"));

  CHECK(validUsername("alice") && validUsername("a.b-c_d@e") && validUsername("u1"));
  CHECK(!validUsername("") && !validUsername(".hidden") && !validUsername("-x") &&
        !validUsername("a/b") && !validUsername("a b") && !validUsername(std::string(65, 'a')) &&
        !validUsername("a\nb"));

  // Requests survive a round trip through the client's serialiser.
  for (const char* line : {R"({"cmd":"auth","username":"alice","service":"sudo","remote":true})",
                           R"({"cmd":"enroll","username":"alice","count":7})",
                           R"({"cmd":"enroll","username":"alice","count":7,"name":"Me"})",
                           R"({"cmd":"enroll","username":"alice","count":7,"face":"9"})",
                           R"({"cmd":"remove","username":"alice","id":"5"})",
                           R"({"cmd":"remove","username":"alice","face":"5"})",
                           R"({"cmd":"rename","username":"alice","face":"5","name":"Me"})",
                           R"({"cmd":"set-encryption","value":"off"})", R"({"cmd":"status"})",
                           R"({"cmd":"migrate"})"}) {
    const Request a = parseOk(line);
    const Request b = parseOk(requestToJson(a));
    CHECK(a.cmd == b.cmd && a.username == b.username && a.service == b.service && a.id == b.id &&
          a.face == b.face && a.name == b.name && a.value == b.value && a.remote == b.remote &&
          a.count == b.count);
  }
  CHECK(toLine(json{{"a", 1}}) == "{\"a\":1}\n");
  CHECK(errorReply("x") == (json{{"result", "error"}, {"detail", "x"}}));
}

void testAuthorization() {
  const uid_t kRoot = 0, kAlice = 1000, kBob = 1001;
  const UidLookup lookup = [&](const std::string& name) -> std::optional<uid_t> {
    if (name == "alice")
      return kAlice;
    if (name == "bob")
      return kBob;
    return std::nullopt;
  };
  auto request = [](Cmd cmd, const std::string& user) {
    Request r;
    r.cmd = cmd;
    r.username = user;
    return r;
  };

  // Root may do everything, for any user.
  for (Cmd cmd : {Cmd::kAuth, Cmd::kTest, Cmd::kEnroll, Cmd::kList, Cmd::kRemove, Cmd::kRename,
                  Cmd::kClear, Cmd::kMigrate, Cmd::kSetEncryption, Cmd::kStatus}) {
    CHECK(isAuthorized(request(cmd, "alice"), kRoot, kRoot, lookup));
  }
  // The user may authenticate and list themselves, nothing else.
  CHECK(isAuthorized(request(Cmd::kAuth, "alice"), kAlice, kRoot, lookup));
  CHECK(isAuthorized(request(Cmd::kList, "alice"), kAlice, kRoot, lookup));
  CHECK(isAuthorized(request(Cmd::kStatus, ""), kAlice, kRoot, lookup));
  for (Cmd cmd :
       {Cmd::kTest, Cmd::kEnroll, Cmd::kRemove, Cmd::kRename, Cmd::kClear, Cmd::kMigrate,
        Cmd::kSetEncryption}) {
    CHECK(!isAuthorized(request(cmd, "alice"), kAlice, kRoot, lookup));
  }
  // Not for somebody else, nor for a user that does not exist.
  CHECK(!isAuthorized(request(Cmd::kAuth, "bob"), kAlice, kRoot, lookup));
  CHECK(!isAuthorized(request(Cmd::kList, "bob"), kAlice, kRoot, lookup));
  CHECK(!isAuthorized(request(Cmd::kAuth, "ghost"), kAlice, kRoot, lookup));
  CHECK(!isAuthorized(request(Cmd::kList, "ghost"), 4242, kRoot, lookup));
  // Anyone may ask for the status.
  CHECK(isAuthorized(request(Cmd::kStatus, ""), 4242, kRoot, lookup));
  // "root" is configurable for the tests.
  CHECK(isAuthorized(request(Cmd::kEnroll, "alice"), kAlice, kAlice, lookup));
}

void testRateLimiter() {
  using Clock = RateLimiter::Clock;
  const Clock::time_point t0 = Clock::now();
  const auto sec = [&](int s) { return t0 + std::chrono::seconds(s); };
  RateLimiter limiter;  // 5 failures in 5 minutes -> blocked for 2 minutes

  CHECK(!limiter.blocked("alice", sec(0)) && !limiter.hasRecentFailures(sec(0)));
  for (int i = 0; i < 4; i++) limiter.recordFailure("alice", sec(i));
  CHECK(!limiter.blocked("alice", sec(10)));
  CHECK(limiter.hasRecentFailures(sec(10)));
  limiter.recordFailure("alice", sec(10));
  CHECK(limiter.blocked("alice", sec(11)));
  CHECK(!limiter.blocked("bob", sec(11)));  // per user
  CHECK(limiter.blocked("alice", sec(10 + 119)));
  CHECK(!limiter.blocked("alice", sec(10 + 120)));  // two minutes later it is over
  CHECK(limiter.hasRecentFailures(sec(10 + 100)));

  // Failures spread over more than five minutes never add up.
  RateLimiter slow;
  for (int i = 0; i < 20; i++) slow.recordFailure("carol", sec(i * 100));
  CHECK(!slow.blocked("carol", sec(20 * 100)));

  // A match forgets the failures.
  RateLimiter forgiving;
  for (int i = 0; i < 4; i++) forgiving.recordFailure("dave", sec(i));
  forgiving.recordSuccess("dave");
  forgiving.recordFailure("dave", sec(5));
  CHECK(!forgiving.blocked("dave", sec(6)));
  forgiving.recordSuccess("dave");
  CHECK(!forgiving.hasRecentFailures(sec(7)));

  // Old failures age out of "recent".
  RateLimiter aging;
  aging.recordFailure("erin", sec(0));
  CHECK(aging.hasRecentFailures(sec(299)));
  CHECK(!aging.hasRecentFailures(sec(301)));
}

void testSessionGate() {
  SessionGate gate;
  SessionGate::Lock first = gate.acquire(std::chrono::milliseconds(10));
  CHECK(static_cast<bool>(first));
  const auto begin = std::chrono::steady_clock::now();
  SessionGate::Lock second = gate.acquire(std::chrono::milliseconds(150));
  const auto waited = std::chrono::steady_clock::now() - begin;
  CHECK(!second);
  CHECK(waited >= std::chrono::milliseconds(140));  // it did wait
  first = SessionGate::Lock();                      // released
  CHECK(static_cast<bool>(gate.acquire(std::chrono::milliseconds(10))));

  // A waiter gets the gate when it is released in time.
  SessionGate::Lock held = gate.acquire(std::chrono::milliseconds(10));
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    held = SessionGate::Lock();
  });
  CHECK(static_cast<bool>(gate.acquire(std::chrono::milliseconds(2000))));
  releaser.join();
}

void testSocketActivation() {
  CHECK(parseListenFds("1234", "1", 1234) == 3);
  CHECK(parseListenFds("1234", "2", 1234) == 3);
  CHECK(parseListenFds("1234", "1", 999) == -1);  // meant for another process
  CHECK(parseListenFds("1234", "0", 1234) == -1);
  CHECK(parseListenFds("abc", "1", 1234) == -1);
  CHECK(parseListenFds("1234", "x", 1234) == -1);
  CHECK(parseListenFds(nullptr, "1", 1234) == -1);
  CHECK(parseListenFds("1234", nullptr, 1234) == -1);
}

// Engine tests that do not need models or a camera: the store-facing commands, status, the
// ignore cases of auth and the busy gate.
struct EngineFixture {
  TempDir dir;
  TempDir hw;
  FakeCrypto crypto;
  TemplateStore store;
  std::function<std::unique_ptr<FrameSource>(const std::string&, std::string&)> opener;
  std::unique_ptr<Engine> engine;

  EngineFixture(bool tpm2, bool secure_boot, const std::string& config_body,
                std::chrono::milliseconds busy_wait = std::chrono::milliseconds(200))
      : store(dir.file("templates"), crypto) {
    lhc_test::fakeHardware(hw, tpm2, secure_boot);
    dir.write("config.yaml", config_body);
    opener = [](const std::string&, std::string& error) -> std::unique_ptr<FrameSource> {
      error = "no camera in this test";
      return nullptr;
    };
    EngineOptions options;
    options.config_path = dir.file("config.yaml");
    options.models_dir = dir.file("no-models");
    options.faces_dir = dir.file("faces");
    options.hardware_root = hw.path();
    options.busy_wait = busy_wait;
    engine = std::make_unique<Engine>(
        options, store, [this](const std::string& d, std::string& e) { return opener(d, e); });
  }

  json call(const std::string& line) {
    Request r;
    std::string error;
    CHECK(parseRequest(line, r, error));
    return engine->handle(r, [](const json&) { return true; });
  }

  void enrol(const std::string& user, Storage as,
             const std::string& model = "edgeface_s_gamma_05.onnx") {
    Template t;
    t.model = model;
    t.dim = 2;
    t.entries.push_back({"1000", 5, {1.0f, 0.0f}});
    t.entries.push_back({"2000", 6, {0.0f, 1.0f}});
    std::string error;
    CHECK(store.save(user, t, as, error));
  }
};

bool existsFile(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void testEngineStatusAndSetEncryption() {
  // TPM 2.0 and Secure Boot on, setting auto: sealed to Secure Boot.
  {
    EngineFixture f(true, true, "camera: /dev/video2\n");
    CHECK(
        f.call(R"({"cmd":"status"})") ==
        (json{
            {"tpm2", true}, {"secure_boot", true}, {"setting", "auto"}, {"effective", "tpm-sb"}}));
  }
  // TPM but no Secure Boot: auto is off.
  {
    EngineFixture f(true, false, "storage:\n  tpm_encryption: auto\n");
    CHECK(f.call(R"({"cmd":"status"})")["effective"] == "none");
  }
  {
    EngineFixture f(true, false, "storage:\n  tpm_encryption: on\n");
    const json s = f.call(R"({"cmd":"status"})");
    CHECK(s["effective"] == "tpm" && s["setting"] == "on" && s["secure_boot"] == false);
  }
  {
    EngineFixture f(false, false, "storage:\n  tpm_encryption: on\n");
    const json s = f.call(R"({"cmd":"status"})");
    CHECK(s["effective"] == "none" && s["tpm2"] == false);  // "on" without a TPM acts like off
  }

  // set-encryption without a TPM: error, config untouched.
  {
    EngineFixture f(false, true, "camera: /dev/video2\n");
    f.enrol("alice", Storage::kNone);
    const json r = f.call(R"({"cmd":"set-encryption","value":"on"})");
    CHECK(r == errorReply("no TPM 2.0"));
    CHECK(!existsFile(f.dir.file("templates/alice.tpm.cred")));
    CHECK(f.call(R"({"cmd":"status"})")["setting"] == "auto");
    // "off" and "auto" are fine without one.
    CHECK(f.call(R"({"cmd":"set-encryption","value":"off"})")["effective"] == "none");
  }

  // set-encryption with a TPM converts everybody, and back.
  {
    EngineFixture f(true, true, "camera: /dev/video2\nstorage:\n  tpm_encryption: off\n");
    f.enrol("alice", Storage::kNone);
    f.enrol("bob", Storage::kNone);
    json r = f.call(R"({"cmd":"set-encryption","value":"on"})");
    CHECK(r["result"] == "ok" && r["effective"] == "tpm-sb" && r["converted"] == 2);
    CHECK(existsFile(f.dir.file("templates/alice.tpm-sb.cred")) &&
          existsFile(f.dir.file("templates/bob.tpm-sb.cred")));
    CHECK(!existsFile(f.dir.file("templates/alice.json")));
    CHECK(f.call(R"({"cmd":"status"})")["setting"] == "on");
    CHECK(f.call(R"({"cmd":"list","username":"alice"})")["encryption"] == "tpm-sb");

    r = f.call(R"({"cmd":"set-encryption","value":"off"})");
    CHECK(r["effective"] == "none" && r["converted"] == 2);
    CHECK(existsFile(f.dir.file("templates/alice.json")));
    CHECK(f.call(R"({"cmd":"list","username":"bob"})")["encryption"] == "none");
    // The rest of the config survived.
    CHECK(parseConfig([&] {
            std::string t;
            lhc_test::readFileForTest(f.dir.file("config.yaml"), t);
            return t;
          }())
              .camera == "/dev/video2");

    // Nothing to convert the second time.
    CHECK(f.call(R"({"cmd":"set-encryption","value":"off"})")["converted"] == 0);

    // Undecryptable templates are reported, not lost.
    r = f.call(R"({"cmd":"set-encryption","value":"on"})");
    f.crypto.fail_decrypt = true;
    r = f.call(R"({"cmd":"set-encryption","value":"off"})");
    CHECK(r["result"] == "ok" && r["converted"] == 0 && r["failed"].size() == 2);
    CHECK(existsFile(f.dir.file("templates/alice.tpm-sb.cred")));
  }
}

void testEngineReconcile() {
  // Config says on, files are plain (edited by hand): the startup reconcile seals them.
  {
    EngineFixture f(true, false, "storage:\n  tpm_encryption: on\n");
    f.enrol("alice", Storage::kNone);
    f.enrol("bob", Storage::kTpmSb);
    f.engine->reconcileAll();
    CHECK(existsFile(f.dir.file("templates/alice.tpm.cred")) &&
          !existsFile(f.dir.file("templates/alice.json")));
    CHECK(existsFile(f.dir.file("templates/bob.tpm.cred")) &&
          !existsFile(f.dir.file("templates/bob.tpm-sb.cred")));
  }
  // Secure Boot switched on later: a .tpm.cred is re-sealed as .tpm-sb.cred.
  {
    EngineFixture f(true, true, "storage:\n  tpm_encryption: auto\n");
    f.enrol("alice", Storage::kTpm);
    f.engine->reconcileAll();
    CHECK(existsFile(f.dir.file("templates/alice.tpm-sb.cred")) &&
          !existsFile(f.dir.file("templates/alice.tpm.cred")));
    CHECK(f.crypto.last_bound_secure_boot);
  }
  // Switched to off: back to plain.
  {
    EngineFixture f(true, true, "storage:\n  tpm_encryption: off\n");
    f.enrol("alice", Storage::kTpmSb);
    f.engine->reconcileAll();
    CHECK(existsFile(f.dir.file("templates/alice.json")));
  }
  // A template that cannot be opened stays put and is reported as needing re-enrolment.
  {
    EngineFixture f(true, true, "storage:\n  tpm_encryption: off\n");
    f.enrol("alice", Storage::kTpmSb);
    f.crypto.fail_decrypt = true;
    f.engine->reconcileAll();
    CHECK(existsFile(f.dir.file("templates/alice.tpm-sb.cred")));
    const json r = f.call(R"({"cmd":"list","username":"alice"})");
    CHECK(r["result"] == "needs_reenrol");
  }
  // Before use (list) the template is also converted, without a restart.
  {
    EngineFixture f(true, true, "storage:\n  tpm_encryption: auto\n");
    f.enrol("alice", Storage::kNone);
    const json r = f.call(R"({"cmd":"list","username":"alice"})");
    CHECK(r["result"] == "ok" && r["encryption"] == "tpm-sb");
  }
}

void testEngineListRemoveClear() {
  EngineFixture f(false, false, "camera: /dev/video2\n");
  CHECK(f.call(R"({"cmd":"list","username":"alice"})") == (json{{"result", "not_enrolled"}}));
  f.enrol("alice", Storage::kNone);

  json r = f.call(R"({"cmd":"list","username":"alice"})");
  CHECK(r["result"] == "ok" && r["encryption"] == "none" &&
        r["model"] == "edgeface_s_gamma_05.onnx");
  // Pictures saved before faces existed all belong to one face, named after the first.
  CHECK(r["entries"].size() == 2 &&
        r["entries"][0] == (json{{"id", "1000"}, {"created", 5}, {"face", "1000"}}) &&
        r["entries"][1]["face"] == "1000" && r["names"] == json::object());
  // Never embeddings.
  CHECK(r.dump().find("embedding") == std::string::npos);

  // Faces can be named; only faces that exist.
  CHECK(f.call(R"({"cmd":"rename","username":"alice","face":"1000","name":"Me"})") ==
        (json{{"result", "ok"}}));
  CHECK(f.call(R"({"cmd":"list","username":"alice"})")["names"] == (json{{"1000", "Me"}}));
  CHECK(f.call(R"({"cmd":"rename","username":"alice","face":"2000","name":"x"})") ==
        errorReply("no such face"));
  CHECK(f.call(R"({"cmd":"enroll","username":"alice","face":"2000"})") ==
        errorReply("no such face"));

  CHECK(f.call(R"({"cmd":"remove","username":"alice","id":"1000"})") == (json{{"result", "ok"}}));
  CHECK(f.call(R"({"cmd":"list","username":"alice"})")["entries"].size() == 1);
  CHECK(f.call(R"({"cmd":"remove","username":"alice","id":"1000"})")["result"] == "error");
  CHECK(f.call(R"({"cmd":"clear","username":"alice"})") == (json{{"result", "ok"}}));
  CHECK(f.call(R"({"cmd":"list","username":"alice"})")["result"] == "not_enrolled");

  // A whole face goes at once, with its name.
  f.enrol("carol", Storage::kNone);
  CHECK(f.call(R"({"cmd":"rename","username":"carol","face":"1000","name":"C"})")["result"] ==
        "ok");
  CHECK(f.call(R"({"cmd":"remove","username":"carol","face":"9"})") ==
        errorReply("no such face"));
  CHECK(f.call(R"({"cmd":"remove","username":"carol","face":"1000"})") ==
        (json{{"result", "ok"}}));
  CHECK(f.call(R"({"cmd":"list","username":"carol"})")["result"] == "not_enrolled");

  // A template from another recognition model needs re-enrolment.
  f.enrol("bob", Storage::kNone, "some-other-model.onnx");
  r = f.call(R"({"cmd":"list","username":"bob"})");
  CHECK(r["result"] == "needs_reenrol" &&
        r["detail"].get<std::string>().find("some-other-model") != std::string::npos);
}

void testEngineAuthIgnoreCases() {
  const std::string base = "camera: /dev/video2\nconfirm:\n  enabled: false\n";
  auto result = [](const json& r) { return r["result"].get<std::string>(); };
  auto detail = [](const json& r) { return r.value("detail", std::string()); };
  {
    EngineFixture f(false, false, base + "disabled: true\n");
    f.enrol("alice", Storage::kNone);
    const json r = f.call(R"({"cmd":"auth","username":"alice","service":"sudo"})");
    CHECK(result(r) == "ignore" && detail(r) == "disabled");
  }
  {
    EngineFixture f(false, false, base + "ignore_services: [sddm]\n");
    f.enrol("alice", Storage::kNone);
    CHECK(detail(f.call(R"({"cmd":"auth","username":"alice","service":"sddm"})")) ==
          "service ignored");
  }
  {
    EngineFixture f(false, false, base);
    f.enrol("alice", Storage::kNone);
    const json r = f.call(R"({"cmd":"auth","username":"alice","service":"sudo","remote":true})");
    CHECK(result(r) == "ignore" && detail(r) == "remote session");
    // With abort_if_ssh off the remote flag no longer matters (it reaches the camera step).
    EngineFixture g(false, false, base + "abort_if_ssh: false\n");
    g.enrol("alice", Storage::kNone);
    CHECK(detail(g.call(R"({"cmd":"auth","username":"alice","service":"sudo","remote":true})")) !=
          "remote session");
  }
  {
    EngineFixture f(false, false, base);  // nobody enrolled
    const json r = f.call(R"({"cmd":"auth","username":"alice","service":"sudo"})");
    CHECK(result(r) == "ignore");
    CHECK(result(f.call(R"({"cmd":"test","username":"alice"})")) == "not_enrolled");
  }
  {
    EngineFixture f(false, false, base);
    f.enrol("alice", Storage::kNone, "another-model.onnx");
    CHECK(result(f.call(R"({"cmd":"auth","username":"alice"})")) == "ignore");
    CHECK(result(f.call(R"({"cmd":"test","username":"alice"})")) == "needs_reenrol");
  }
  {
    EngineFixture f(false, false, "confirm:\n  enabled: false\n");  // no camera configured
    f.enrol("alice", Storage::kNone);
    CHECK(result(f.call(R"({"cmd":"auth","username":"alice"})")) == "ignore");
    CHECK(result(f.call(R"({"cmd":"test","username":"alice"})")) == "camera_unavailable");
  }
  {
    EngineFixture f(false, false, base);  // camera cannot be opened
    f.enrol("alice", Storage::kNone);
    const json r = f.call(R"({"cmd":"auth","username":"alice"})");
    CHECK(result(r) == "ignore" && detail(r) == "no camera in this test");
    CHECK(result(f.call(R"({"cmd":"test","username":"alice"})")) == "camera_unavailable");
    CHECK(f.call(R"({"cmd":"enroll","username":"alice","count":2})") ==
          errorReply("camera unavailable: no camera in this test"));
  }
  {
    EngineFixture f(false, false, "camera: /dev/video2\n");
    f.dir.write("config.yaml", "camera: [unclosed\n");  // unusable config
    CHECK(result(f.call(R"({"cmd":"auth","username":"alice"})")) == "ignore");
  }
}

// Two requests want the camera: the second waits, then gets "busy".
void testEngineBusy() {
  EngineFixture f(false, false, "camera: /dev/video2\nconfirm:\n  enabled: false\n",
                  std::chrono::milliseconds(300));
  f.enrol("alice", Storage::kNone);

  std::mutex m;
  std::condition_variable cv;
  bool entered = false, release = false;
  f.opener = [&](const std::string&, std::string& error) -> std::unique_ptr<FrameSource> {
    std::unique_lock<std::mutex> lock(m);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    error = "released";
    return nullptr;
  };

  json first;
  std::thread holder([&] { first = f.call(R"({"cmd":"auth","username":"alice"})"); });
  {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return entered; });
  }

  // Everything that needs the camera or the models is busy; reads of the store are not.
  CHECK(f.call(R"({"cmd":"auth","username":"alice"})") == (json{{"result", "busy"}}));
  CHECK(f.call(R"({"cmd":"test","username":"alice"})") == (json{{"result", "busy"}}));
  CHECK(f.call(R"({"cmd":"enroll","username":"alice","count":1})") == (json{{"result", "busy"}}));
  CHECK(f.call(R"({"cmd":"migrate"})") == (json{{"result", "busy"}}));
  CHECK(f.call(R"({"cmd":"list","username":"alice"})")["result"] == "ok");
  CHECK(f.call(R"({"cmd":"status"})").contains("tpm2"));

  {
    std::lock_guard<std::mutex> lock(m);
    release = true;
  }
  cv.notify_all();
  holder.join();
  CHECK(first["result"] == "ignore" && first["detail"] == "released");
  // And afterwards the camera is free again.
  CHECK(f.call(R"({"cmd":"auth","username":"alice"})")["result"] == "ignore");
}

// Real sockets: SO_PEERCRED decides. The "root" uid is set to one that is not ours, so this
// process is an ordinary user, with a fake passwd lookup in which it is "me".
void testServerPeerCredentials() {
  EngineFixture f(false, false, "camera: /dev/video2\n");
  f.enrol("me", Storage::kNone);
  f.enrol("other", Storage::kNone);

  ServerOptions options;
  options.socket_path = f.dir.file("run/engine.sock");
  options.idle_timeout_s = 0;
  options.root_uid = getuid() + 1;
  options.uid_lookup = [](const std::string& name) -> std::optional<uid_t> {
    if (name == "me")
      return getuid();
    if (name == "other")
      return getuid() + 2;
    return std::nullopt;
  };
  auto server_ptr = std::make_unique<Server>(*f.engine, options);
  Server& server = *server_ptr;
  std::string error;
  CHECK(server.start(error));
  std::thread loop([&] { server.run(); });

  ClientContext context;
  context.socket_path = options.socket_path;
  auto ask = [&](const std::string& line) {
    Request r;
    std::string e;
    CHECK(parseRequest(line, r, e));
    return callDaemon(context, r, 5000);
  };

  CallResult r = ask(R"({"cmd":"status"})");
  CHECK(r.status == CallStatus::kOk && r.reply.contains("effective"));
  r = ask(R"({"cmd":"list","username":"me"})");
  CHECK(r.status == CallStatus::kOk && r.reply["result"] == "ok");
  r = ask(R"({"cmd":"list","username":"other"})");
  CHECK(r.status == CallStatus::kOk && r.reply == errorReply("not allowed"));
  r = ask(R"({"cmd":"auth","username":"other","service":"sudo"})");
  CHECK(r.reply == errorReply("not allowed"));
  r = ask(R"({"cmd":"auth","username":"me","service":"sudo"})");
  CHECK(r.reply["result"] == "ignore");  // allowed; the engine then ignores it (no camera here)
  for (const char* line :
       {R"({"cmd":"clear","username":"me"})", R"({"cmd":"enroll","username":"me"})",
        R"({"cmd":"test","username":"me"})", R"({"cmd":"migrate"})",
        R"({"cmd":"remove","username":"me","id":"1000"})",
        R"({"cmd":"rename","username":"me","face":"1000","name":"x"})",
        R"({"cmd":"set-encryption","value":"on"})"}) {
    r = ask(line);
    CHECK(r.status == CallStatus::kOk && r.reply == errorReply("not root"));
  }
  CHECK(ask(R"({"cmd":"list","username":"me"})").reply["result"] == "ok");  // clear did not run

  // Malformed input gets an error line, and a silent client does not hang the daemon.
  auto raw = [&](const std::string& bytes) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, options.socket_path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ssize_t ignored = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    (void)ignored;
    shutdown(fd, SHUT_WR);
    char buf[256] = {};
    const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
  };
  CHECK(raw("{\"cmd\":\"fly\"}\n") == toLine(errorReply("unknown command")));
  CHECK(raw("garbage\n") == toLine(errorReply("malformed request")));
  CHECK(raw("{\"cmd\":\"status\"") == "");   // closed without a newline: no answer
  CHECK(raw(std::string(6000, 'x')) == "");  // too long: dropped
  CHECK(ask(R"({"cmd":"status"})").status == CallStatus::kOk);

  server.stop();
  loop.join();
  server_ptr.reset();
  // The socket is gone, so the next client sees an unreachable daemon.
  CHECK(!existsFile(options.socket_path));
  CHECK(ask(R"({"cmd":"status"})").status == CallStatus::kUnreachable);
}

// Idle exit: with no connection the server returns by itself, unless failures are recent.
void testServerIdleExit() {
  EngineFixture f(false, false, "camera: /dev/video2\n");
  ServerOptions options;
  options.socket_path = f.dir.file("idle.sock");
  options.idle_timeout_s = 1;
  Server server(*f.engine, options);
  std::string error;
  CHECK(server.start(error));
  const auto begin = std::chrono::steady_clock::now();
  server.run();
  const auto took = std::chrono::steady_clock::now() - begin;
  CHECK(took >= std::chrono::milliseconds(900) && took < std::chrono::seconds(5));
}

}  // namespace

void testDaemon() {
  testProtocol();
  testAuthorization();
  testRateLimiter();
  testSessionGate();
  testSocketActivation();
  testEngineStatusAndSetEncryption();
  testEngineReconcile();
  testEngineListRemoveClear();
  testEngineAuthIgnoreCases();
  testEngineBusy();
  testServerPeerCredentials();
  testServerIdleExit();
}
