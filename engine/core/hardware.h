#pragma once

#include <string>

namespace lhc {

// What the PC offers for protecting face templates. The app uses the same rules (DESIGN.md).
struct Hardware {
  bool tpm2 = false;         // /dev/tpmrm0 exists and the TPM reports version 2
  bool secure_boot = false;  // the SecureBoot EFI variable says 1 (missing = off)
};

// `root` is prefixed to every path read (dev, sys), so tests can fake the tree.
Hardware detectHardware(const std::string& root = "");

// storage.tpm_encryption
enum class TpmSetting { kAuto, kOn, kOff };

const char* settingName(TpmSetting setting);
bool parseSetting(const std::string& text, TpmSetting& out);

// Whether templates are TPM-encrypted: "on" needs a TPM 2.0 (without one it behaves like off),
// "auto" needs a TPM 2.0 and Secure Boot, "off" never.
bool tpmEncryptionEffective(TpmSetting setting, const Hardware& hardware);

}  // namespace lhc
