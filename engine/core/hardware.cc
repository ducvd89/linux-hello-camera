#include "core/hardware.h"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>

namespace lhc {

namespace {

constexpr const char* kTpmDevice = "/dev/tpmrm0";
constexpr const char* kTpmVersionFile = "/sys/class/tpm/tpm0/tpm_version_major";
constexpr const char* kSecureBootVar =
    "/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c";

bool exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

}  // namespace

Hardware detectHardware(const std::string& root) {
  Hardware hw;

  std::ifstream version(root + kTpmVersionFile);
  std::string major;
  version >> major;
  hw.tpm2 = exists(root + kTpmDevice) && major == "2";

  // An EFI variable file is 4 bytes of attributes followed by the value.
  std::ifstream var(root + kSecureBootVar, std::ios::binary);
  unsigned char bytes[5] = {};
  var.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
  hw.secure_boot = var.gcount() == 5 && bytes[4] == 1;
  return hw;
}

const char* settingName(TpmSetting setting) {
  switch (setting) {
    case TpmSetting::kOn:
      return "on";
    case TpmSetting::kOff:
      return "off";
    case TpmSetting::kAuto:
      break;
  }
  return "auto";
}

bool parseSetting(const std::string& text, TpmSetting& out) {
  for (TpmSetting s : {TpmSetting::kAuto, TpmSetting::kOn, TpmSetting::kOff}) {
    if (text == settingName(s)) {
      out = s;
      return true;
    }
  }
  return false;
}

bool tpmEncryptionEffective(TpmSetting setting, const Hardware& hardware) {
  switch (setting) {
    case TpmSetting::kOn:
      return hardware.tpm2;
    case TpmSetting::kAuto:
      return hardware.tpm2 && hardware.secure_boot;
    case TpmSetting::kOff:
      break;
  }
  return false;
}

}  // namespace lhc
