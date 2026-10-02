#pragma once

#include <stdlib.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace lhc_test {

// A scratch directory removed on destruction.
class TempDir {
 public:
  TempDir() {
    std::string pattern = (std::filesystem::temp_directory_path() / "lhc-test-XXXXXX").string();
    path_ = mkdtemp(pattern.data());
  }
  ~TempDir() { std::filesystem::remove_all(path_); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string file(const std::string& name) const { return path_ + "/" + name; }

  void write(const std::string& name, const std::string& content) const {
    std::filesystem::create_directories(std::filesystem::path(file(name)).parent_path());
    std::ofstream(file(name), std::ios::binary) << content;
  }

 private:
  std::string path_;
};

inline bool readFileForTest(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  out.assign(std::istreambuf_iterator<char>(f), {});
  return static_cast<bool>(f);
}

// Builds the files detectHardware() reads under root/<prefix>.
inline void fakeHardware(const TempDir& root, bool tpm2, bool secure_boot,
                         const std::string& prefix = "") {
  if (tpm2) {
    root.write(prefix + "dev/tpmrm0", "");
    root.write(prefix + "sys/class/tpm/tpm0/tpm_version_major", "2\n");
  }
  root.write(prefix + "sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",
             std::string("\x06\x00\x00\x00", 4) + std::string(1, secure_boot ? '\x01' : '\x00'));
}

}  // namespace lhc_test
