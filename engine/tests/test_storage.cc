#include <sys/stat.h>

#include <fstream>

#include "core/base64.h"
#include "core/config.h"
#include "core/hardware.h"
#include "face/enrolled_faces.h"
#include "store/migrate.h"
#include "store/systemd_creds.h"
#include "store/template_store.h"
#include "tests/fake_crypto.h"
#include "tests/temp_dir.h"
#include "tests/test.h"

using namespace lhc;
using lhc_test::FakeCrypto;
using lhc_test::TempDir;

namespace {

Template sampleTemplate(const std::string& model = "edgeface.onnx") {
  Template t;
  t.model = model;
  t.dim = 4;
  t.entries.push_back({"1000", 1, {0.5f, -1.0f, 3.25f, 1e-7f}});
  t.entries.push_back({"2000", 2, {0.0f, 1.0f, -0.0f, 123456.0f}});
  return t;
}

unsigned modeOf(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

bool exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void testBase64() {
  auto enc = [](const std::string& s) {
    return base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  CHECK(enc("") == "");
  CHECK(enc("f") == "Zg==");
  CHECK(enc("fo") == "Zm8=");
  CHECK(enc("foo") == "Zm9v");
  CHECK(enc("foob") == "Zm9vYg==");

  std::vector<uint8_t> out;
  CHECK(base64Decode("Zm9vYg==", out) && std::string(out.begin(), out.end()) == "foob");
  CHECK(base64Decode("", out) && out.empty());
  CHECK(!base64Decode("Zm9", out));       // bad length
  CHECK(!base64Decode("Zm9v!A==", out));  // bad character
  CHECK(!base64Decode("Zg=A", out));      // data after padding
  CHECK(!base64Decode("====", out));

  std::vector<uint8_t> all(256);
  for (int i = 0; i < 256; i++) all[i] = static_cast<uint8_t>(i);
  CHECK(base64Decode(base64Encode(all.data(), all.size()), out) && out == all);
}

void testTemplateJson() {
  const Template t = sampleTemplate();
  const std::string json = templateToJson(t);
  Template back;
  std::string error;
  CHECK(templateFromJson(json, back, error));
  CHECK(back.model == t.model && back.dim == 4 && back.entries.size() == 2);
  CHECK(back.entries[0].id == "1000" && back.entries[0].created == 1);
  CHECK(back.entries[0].embedding == t.entries[0].embedding);  // bit exact
  CHECK(back.entries[1].embedding == t.entries[1].embedding);

  // The embedding really is base64 of little-endian float32: 1.0f = 00 00 80 3f.
  Template one;
  one.model = "m";
  one.dim = 1;
  one.entries.push_back({"1", 1, {1.0f}});
  CHECK(templateToJson(one).find("\"AACAPw==\"") != std::string::npos);

  // Faces and their names survive; pictures from before faces existed join the first one's face.
  Template faces = sampleTemplate();
  faces.entries[0].face = "1000";
  faces.entries[1].face = "2000";
  faces.names["2000"] = "Đức";
  CHECK(templateFromJson(templateToJson(faces), back, error));
  CHECK(back.entries[0].face == "1000" && back.entries[1].face == "2000" &&
        back.names == faces.names);
  CHECK(templateFromJson("{\"version\":1,\"model\":\"m\",\"dim\":1,\"entries\":["
                         "{\"id\":\"7\",\"created\":1,\"embedding\":\"AACAPw==\"},"
                         "{\"id\":\"8\",\"created\":1,\"embedding\":\"AACAPw==\"}]}",
                         back, error));
  CHECK(back.entries[0].face == "7" && back.entries[1].face == "7" && back.names.empty());

  CHECK(!templateFromJson("not json", back, error));
  CHECK(!templateFromJson("{\"version\":2,\"model\":\"m\",\"dim\":1,\"entries\":[]}", back, error));
  CHECK(
      !templateFromJson("{\"version\":1,\"model\":\"m\",\"dim\":2,\"entries\":["
                        "{\"id\":\"1\",\"created\":1,\"embedding\":\"AACAPw==\"}]}",
                        back, error));  // one float where dim says two
}

void testHardwareDetection() {
  {
    TempDir root;
    lhc_test::fakeHardware(root, true, true);
    const Hardware hw = detectHardware(root.path());
    CHECK(hw.tpm2 && hw.secure_boot);
  }
  {
    TempDir root;
    lhc_test::fakeHardware(root, true, false);
    const Hardware hw = detectHardware(root.path());
    CHECK(hw.tpm2 && !hw.secure_boot);
  }
  {
    TempDir root;  // no TPM files at all, no EFI variable
    const Hardware hw = detectHardware(root.path());
    CHECK(!hw.tpm2 && !hw.secure_boot);
  }
  {
    TempDir root;  // device present but a TPM 1.2
    root.write("dev/tpmrm0", "");
    root.write("sys/class/tpm/tpm0/tpm_version_major", "1\n");
    CHECK(!detectHardware(root.path()).tpm2);
  }
  {
    TempDir root;  // version file without the device node
    root.write("sys/class/tpm/tpm0/tpm_version_major", "2\n");
    CHECK(!detectHardware(root.path()).tpm2);
  }
  {
    TempDir root;  // a truncated EFI variable
    root.write("sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",
               "\x06\x00");
    CHECK(!detectHardware(root.path()).secure_boot);
  }
}

void testStorageSelection() {
  // Every combination of setting x TPM x Secure Boot.
  struct Case {
    TpmSetting setting;
    bool tpm2, sb;
    Storage expected;
  } cases[] = {
      {TpmSetting::kAuto, false, false, Storage::kNone},
      {TpmSetting::kAuto, false, true, Storage::kNone},
      {TpmSetting::kAuto, true, false, Storage::kNone},  // TPM but no Secure Boot: auto stays off
      {TpmSetting::kAuto, true, true, Storage::kTpmSb},
      {TpmSetting::kOn, false, false, Storage::kNone},  // "on" without a TPM behaves like off
      {TpmSetting::kOn, false, true, Storage::kNone},
      {TpmSetting::kOn, true, false, Storage::kTpm},
      {TpmSetting::kOn, true, true, Storage::kTpmSb},
      {TpmSetting::kOff, false, false, Storage::kNone},
      {TpmSetting::kOff, false, true, Storage::kNone},
      {TpmSetting::kOff, true, false, Storage::kNone},
      {TpmSetting::kOff, true, true, Storage::kNone},
  };
  for (const Case& c : cases) {
    Hardware hw;
    hw.tpm2 = c.tpm2;
    hw.secure_boot = c.sb;
    CHECK(chooseStorage(c.setting, hw) == c.expected);
  }
  CHECK(std::string(storageName(Storage::kNone)) == "none");
  CHECK(std::string(storageName(Storage::kTpm)) == "tpm");
  CHECK(std::string(storageName(Storage::kTpmSb)) == "tpm-sb");

  TpmSetting s;
  CHECK(parseSetting("on", s) && s == TpmSetting::kOn);
  CHECK(parseSetting("off", s) && s == TpmSetting::kOff);
  CHECK(parseSetting("auto", s) && s == TpmSetting::kAuto);
  CHECK(!parseSetting("yes", s));
}

void testStoreBasics() {
  TempDir dir;
  FakeCrypto crypto;
  TemplateStore store(dir.file("templates"), crypto);
  const std::string model = "edgeface.onnx";

  CHECK(store.load("alice", model, Storage::kNone).status == LoadStatus::kNotEnrolled);

  std::string error;
  CHECK(store.save("alice", sampleTemplate(), Storage::kTpmSb, error));
  // Directory 0700 and file 0600, the file is the sealed form and no plaintext sits next to it.
  CHECK(modeOf(dir.file("templates")) == 0700);
  CHECK(modeOf(dir.file("templates/alice.tpm-sb.cred")) == 0600);
  CHECK(!exists(dir.file("templates/alice.json")));
  CHECK(crypto.last_bound_secure_boot);
  CHECK(store.users() == std::vector<std::string>{"alice"});

  // No stray temp file is left behind by the atomic write.
  int files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir.file("templates"))) {
    (void)e;
    files++;
  }
  CHECK(files == 1);

  LoadResult r = store.load("alice", model, Storage::kTpmSb);
  CHECK(r.status == LoadStatus::kOk && r.storage == Storage::kTpmSb);
  CHECK(r.tmpl.entries.size() == 2 && r.tmpl.entries[1].embedding[3] == 123456.0f);

  // Plain kind: JSON on disk, 0600.
  CHECK(store.save("bob", sampleTemplate(), Storage::kNone, error));
  CHECK(modeOf(dir.file("templates/bob.json")) == 0600);
  CHECK(store.users() == (std::vector<std::string>{"alice", "bob"}));

  // A credential made for one user does not open as another (the name ties it).
  std::filesystem::copy_file(dir.file("templates/alice.tpm-sb.cred"),
                             dir.file("templates/mallory.tpm-sb.cred"));
  CHECK(store.load("mallory", model, Storage::kTpmSb).status == LoadStatus::kNeedsReenrol);

  // Entries can be removed one by one; the last one removes the file.
  bool found = false;
  CHECK(store.removeEntry("alice", "1000", model, Storage::kTpmSb, found, error) && found);
  CHECK(store.load("alice", model, Storage::kTpmSb).tmpl.entries.size() == 1);
  CHECK(store.removeEntry("alice", "nope", model, Storage::kTpmSb, found, error) && !found);
  CHECK(store.removeEntry("alice", "2000", model, Storage::kTpmSb, found, error) && found);
  CHECK(store.load("alice", model, Storage::kTpmSb).status == LoadStatus::kNotEnrolled);
  CHECK(!exists(dir.file("templates/alice.tpm-sb.cred")));

  // A whole face can be removed; its name goes with it.
  Template two = sampleTemplate();
  two.entries[0].face = "1000";
  two.entries[1].face = "2000";
  two.names = {{"1000", "A"}, {"2000", "B"}};
  CHECK(store.save("bob", two, Storage::kNone, error));
  CHECK(store.removeEntry("bob", "", model, Storage::kNone, found, error, "1000") && found);
  const LoadResult left = store.load("bob", model, Storage::kNone);
  CHECK(left.tmpl.entries.size() == 1 && left.tmpl.entries[0].face == "2000" &&
        left.tmpl.names == (std::map<std::string, std::string>{{"2000", "B"}}));
  CHECK(store.removeEntry("bob", "", model, Storage::kNone, found, error, "1000") && !found);

  CHECK(store.clear("bob", error));
  CHECK(!exists(dir.file("templates/bob.json")));
  CHECK(store.clear("bob", error));  // already gone is fine
}

void testNeedsReenrol() {
  TempDir dir;
  FakeCrypto crypto;
  TemplateStore store(dir.file("t"), crypto);
  std::string error;
  CHECK(store.save("alice", sampleTemplate("old-model.onnx"), Storage::kTpm, error));

  // Another recognition model is configured now.
  LoadResult r = store.load("alice", "new-model.onnx", Storage::kTpm);
  CHECK(r.status == LoadStatus::kNeedsReenrol);
  CHECK(r.detail.find("old-model.onnx") != std::string::npos);

  // Decryption fails (TPM cleared, PCR 7 changed).
  crypto.fail_decrypt = true;
  r = store.load("alice", "old-model.onnx", Storage::kTpm);
  CHECK(r.status == LoadStatus::kNeedsReenrol);
  crypto.fail_decrypt = false;
  CHECK(store.load("alice", "old-model.onnx", Storage::kTpm).status == LoadStatus::kOk);

  // A plain file that is not a template.
  dir.write("t/carol.json", "{ broken");
  CHECK(store.load("carol", "m", Storage::kNone).status == LoadStatus::kNeedsReenrol);

  // Entry removal needs a readable template.
  bool found;
  crypto.fail_decrypt = true;
  CHECK(!store.removeEntry("alice", "1000", "old-model.onnx", Storage::kTpm, found, error));
  crypto.fail_decrypt = false;

  // A failing encrypt leaves nothing behind and keeps the old file.
  crypto.fail_encrypt = true;
  CHECK(!store.save("alice", sampleTemplate("old-model.onnx"), Storage::kTpmSb, error));
  CHECK(!exists(dir.file("t/alice.tpm-sb.cred")));
  CHECK(exists(dir.file("t/alice.tpm.cred")));
}

void testConversions() {
  TempDir dir;
  FakeCrypto crypto;
  TemplateStore store(dir.file("t"), crypto);
  std::string error;
  const std::string model = "edgeface.onnx";
  CHECK(store.save("alice", sampleTemplate(model), Storage::kNone, error));

  // Between every pair of kinds, in place, one file at a time, content intact.
  const Storage order[] = {Storage::kTpm, Storage::kTpmSb, Storage::kNone, Storage::kTpmSb,
                           Storage::kTpm, Storage::kNone,  Storage::kTpm};
  Storage current = Storage::kNone;
  for (Storage target : order) {
    CHECK(store.convert("alice", target, error) == ConvertStatus::kConverted);
    int present = 0;
    for (Storage s : kAllStorages) {
      const char* suffix = s == Storage::kNone  ? ".json"
                           : s == Storage::kTpm ? ".tpm.cred"
                                                : ".tpm-sb.cred";
      const bool has = exists(dir.file(std::string("t/alice") + suffix));
      CHECK(has == (s == target));
      present += has;
    }
    CHECK(present == 1);
    const LoadResult r = store.load("alice", model, target);
    CHECK(r.status == LoadStatus::kOk && r.storage == target && r.tmpl.entries.size() == 2);
    CHECK(r.tmpl.entries[0].embedding == sampleTemplate(model).entries[0].embedding);
    CHECK(store.convert("alice", target, error) == ConvertStatus::kUnchanged);
    current = target;
  }
  (void)current;
  CHECK(crypto.last_bound_secure_boot == false);  // the last encrypt was kTpm

  // Content that is not decryptable is not converted and stays where it is.
  CHECK(store.convert("alice", Storage::kTpmSb, error) == ConvertStatus::kConverted);
  crypto.fail_decrypt = true;
  CHECK(store.convert("alice", Storage::kNone, error) == ConvertStatus::kFailed);
  CHECK(exists(dir.file("t/alice.tpm-sb.cred")));
  CHECK(!exists(dir.file("t/alice.json")));
  crypto.fail_decrypt = false;

  // An interrupted conversion left both files: the wanted one wins, the other goes.
  dir.write("t/alice.json", "stale");
  CHECK(store.convert("alice", Storage::kTpmSb, error) == ConvertStatus::kConverted);
  CHECK(!exists(dir.file("t/alice.json")));
  CHECK(store.load("alice", model, Storage::kTpmSb).status == LoadStatus::kOk);

  // load() with two files present uses the preferred one.
  CHECK(store.save("alice", sampleTemplate(model), Storage::kNone, error));
  dir.write("t/alice.tpm.cred",
            "FAKE:nosb:linux-hello-camera-alice:" + templateToJson(sampleTemplate(model)));
  CHECK(store.load("alice", model, Storage::kTpm).storage == Storage::kTpm);
  CHECK(store.load("alice", model, Storage::kNone).storage == Storage::kNone);
}

void testSystemdCredsArguments() {
  SystemdCreds tpm;
  CHECK((tpm.encryptArgs("linux-hello-camera-bob", false) ==
         std::vector<std::string>{"encrypt", "--name=linux-hello-camera-bob",
                                  "--with-key=host+tpm2", "-", "-"}));
  CHECK((tpm.encryptArgs("linux-hello-camera-bob", true) ==
         std::vector<std::string>{"encrypt", "--name=linux-hello-camera-bob",
                                  "--with-key=host+tpm2", "--tpm2-pcrs=7", "-", "-"}));
  CHECK(credentialName("bob") == "linux-hello-camera-bob");
}

// The real systemd-creds, with the null key (no TPM needed), when the tool is installed.
void testRealSystemdCreds() {
  if (!exists("/usr/bin/systemd-creds")) {
    std::printf("skip: no systemd-creds\n");
    return;
  }
  SystemdCreds creds("/usr/bin/systemd-creds", "null", /*allow_null=*/true);
  const std::string secret =
      std::string("embedding data ") + std::string(100000, 'x');  // > pipe buffer
  std::string cred, plain, error;
  CHECK(creds.encrypt("linux-hello-camera-test", secret, false, cred, error));
  CHECK(!cred.empty() && cred.find("embedding") == std::string::npos);
  // systemd refuses to decrypt null-key credentials while Secure Boot is on, whatever
  // --allow-null says; where that applies the round trip can only be shown to fail cleanly.
  if (creds.decrypt("linux-hello-camera-test", cred, plain, error)) {
    CHECK(plain == secret);
    // The name is part of the credential.
    CHECK(!creds.decrypt("linux-hello-camera-other", cred, plain, error));
  } else {
    CHECK(error.find("null key") != std::string::npos);
  }
  // A tool that is not there is an error, not a crash.
  SystemdCreds missing("/nonexistent/systemd-creds");
  CHECK(!missing.encrypt("n", "x", false, cred, error));
}

void testTpmSettingConfig() {
  CHECK(parseConfig("").storage.tpm_encryption == TpmSetting::kAuto);
  CHECK(parseConfig("storage:\n  tpm_encryption: on\n").storage.tpm_encryption == TpmSetting::kOn);
  CHECK(parseConfig("storage:\n  tpm_encryption: off\n").storage.tpm_encryption ==
        TpmSetting::kOff);
  CHECK(parseConfig("storage:\n  tpm_encryption: maybe\n").storage.tpm_encryption ==
        TpmSetting::kAuto);
  CHECK(parseConfig("storage: nonsense\n").storage.tpm_encryption == TpmSetting::kAuto);

  // writeTpmSetting changes only its key and keeps mode 0644.
  TempDir dir;
  dir.write("config.yaml", "schema_version: 1\ncamera: /dev/video2\nconfirm:\n  enabled: false\n");
  std::string error;
  CHECK(writeTpmSetting(dir.file("config.yaml"), TpmSetting::kOn, error));
  Config c = parseConfig([&] {
    std::ifstream f(dir.file("config.yaml"));
    return std::string(std::istreambuf_iterator<char>(f), {});
  }());
  CHECK(c.storage.tpm_encryption == TpmSetting::kOn);
  CHECK(c.camera == "/dev/video2" && !c.confirm.enabled);
  CHECK(modeOf(dir.file("config.yaml")) == 0644);
  CHECK(writeTpmSetting(dir.file("config.yaml"), TpmSetting::kOff, error));
  CHECK(modeOf(dir.file("config.yaml")) == 0644);

  // A missing file is created; a file that is not YAML is refused and left alone.
  CHECK(writeTpmSetting(dir.file("fresh.yaml"), TpmSetting::kAuto, error));
  dir.write("bad.yaml", "camera: [unclosed\n");
  CHECK(!writeTpmSetting(dir.file("bad.yaml"), TpmSetting::kOn, error));
}

// Migration with a fake embedder and fake crypto, on a temp faces directory.
void writeCrop(const std::string& path, uint8_t level) {
  ImageRGB crop(8, 8);
  std::fill(crop.data.begin(), crop.data.end(), level);
  CHECK(writeGreyPng(path, crop));
}

void testMigrate() {
  TempDir dir;
  FakeCrypto crypto;
  TemplateStore store(dir.file("templates"), crypto);
  const std::string faces = dir.file("faces");
  const std::string model = "edgeface.onnx";
  std::filesystem::create_directories(faces + "/alice");
  std::filesystem::create_directories(faces + "/bob");
  writeCrop(faces + "/alice/1790000000100.png", 10);
  writeCrop(faces + "/alice/1790000000200.png", 20);
  writeCrop(faces + "/bob/1790000000300.png", 30);
  dir.write("faces/bob/notes.txt", "not an image");

  // The embedding is the crop's grey level, so the content is checkable.
  const Embedder embed = [](const ImageRGB& crop) {
    return std::vector<float>{static_cast<float>(crop.data[0]), 1.0f};
  };
  const std::string crop_path = faces + "/alice/1790000000100.png";
  const std::vector<std::string> alice_crops = listFaces(faces + "/alice");
  CHECK(alice_crops.size() == 2);

  MigrateResult r = migrateFaces(faces, store, model, Storage::kTpmSb, embed);
  CHECK((r.migrated == std::vector<std::string>{"alice", "bob"}));
  CHECK(r.failed.empty());

  // Templates carry the embeddings, with ids and times from the crop names.
  LoadResult alice = store.load("alice", model, Storage::kTpmSb);
  CHECK(alice.status == LoadStatus::kOk && alice.tmpl.entries.size() == 2);
  CHECK(alice.tmpl.entries[0].id == "1790000000100" && alice.tmpl.entries[0].created == 1790000000);
  CHECK(alice.tmpl.entries[0].embedding[0] == 10.0f && alice.tmpl.entries[1].embedding[0] == 20.0f);
  CHECK(store.load("bob", model, Storage::kTpmSb).tmpl.entries.size() == 1);

  // The pictures are gone and so are the emptied user directories (bob's stray text file is not a
  // crop and is left alone, so his directory stays).
  CHECK(!exists(crop_path));
  CHECK(listFaces(faces + "/alice").empty());
  CHECK(!exists(faces + "/alice"));
  CHECK(listFaces(faces + "/bob").empty());

  // Running again changes nothing.
  r = migrateFaces(faces, store, model, Storage::kTpmSb, embed);
  CHECK(r.migrated.empty() && r.failed.empty());
  CHECK(store.load("alice", model, Storage::kTpmSb).tmpl.entries.size() == 2);

  // New crops for an already migrated user merge; crops already in the template are not doubled
  // (as when a crash happened after the template was written but before the crops were removed).
  std::filesystem::create_directories(faces + "/alice");
  writeCrop(faces + "/alice/1790000000200.png", 20);  // already there
  writeCrop(faces + "/alice/1790000000400.png", 40);
  r = migrateFaces(faces, store, model, Storage::kTpmSb, embed);
  CHECK(r.migrated == std::vector<std::string>{"alice"});
  alice = store.load("alice", model, Storage::kTpmSb);
  CHECK(alice.tmpl.entries.size() == 3 && alice.tmpl.entries[2].id == "1790000000400");
  CHECK(!exists(faces + "/alice"));

  // Crops with no usable face keep their files and are reported.
  std::filesystem::create_directories(faces + "/carol");
  writeCrop(faces + "/carol/1790000000500.png", 50);
  r = migrateFaces(faces, store, model, Storage::kNone,
                   [](const ImageRGB&) { return std::vector<float>(); });
  CHECK(r.migrated.empty() && r.failed.size() == 1 && r.failed[0].first == "carol");
  CHECK(exists(faces + "/carol/1790000000500.png"));
  CHECK(store.load("carol", model, Storage::kNone).status == LoadStatus::kNotEnrolled);

  // A template made with another model is replaced rather than mixed with the new embeddings.
  std::string save_error;
  CHECK(store.save("dave", sampleTemplate("old-model.onnx"), Storage::kNone, save_error));
  std::filesystem::create_directories(faces + "/dave");
  writeCrop(faces + "/dave/1790000000600.png", 60);
  r = migrateFaces(faces, store, model, Storage::kNone, embed);
  CHECK((r.migrated == std::vector<std::string>{"carol", "dave"}));  // carol's crops embed now
  const LoadResult dave = store.load("dave", model, Storage::kNone);
  CHECK(dave.status == LoadStatus::kOk && dave.tmpl.entries.size() == 1 && dave.tmpl.dim == 2);

  // shredFile overwrites before unlinking.
  dir.write("secret.bin", std::string(100000, 'S'));
  CHECK(shredFile(dir.file("secret.bin")));
  CHECK(!exists(dir.file("secret.bin")));
  CHECK(!shredFile(dir.file("missing.bin")));
}

}  // namespace

void testStorage() {
  testBase64();
  testTemplateJson();
  testHardwareDetection();
  testStorageSelection();
  testStoreBasics();
  testNeedsReenrol();
  testConversions();
  testSystemdCredsArguments();
  testRealSystemdCreds();
  testTpmSettingConfig();
  testMigrate();
}
