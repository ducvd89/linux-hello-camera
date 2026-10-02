#pragma once

#include <string>
#include <vector>

#include "store/crypto.h"

namespace lhc {

// Encrypts through `systemd-creds` as a subprocess: no shell, an empty environment, data over
// pipes. Keyed with host+tpm2 (the host secret plus the TPM), so a file can only be decrypted on
// this machine with its TPM; with Secure Boot binding also sealed to PCR 7.
class SystemdCreds : public Crypto {
 public:
  // `key` and `allow_null` are only for tests ("null" key, which needs --allow-null to decrypt).
  explicit SystemdCreds(std::string binary = "/usr/bin/systemd-creds",
                        std::string key = "host+tpm2", bool allow_null = false)
      : binary_(std::move(binary)), key_(std::move(key)), allow_null_(allow_null) {}

  bool encrypt(const std::string& name, const std::string& plain, bool bind_secure_boot,
               std::string& cred, std::string& error) override;
  bool decrypt(const std::string& name, const std::string& cred, std::string& plain,
               std::string& error) override;

  // The systemd-creds arguments for encrypt (public for tests).
  std::vector<std::string> encryptArgs(const std::string& name, bool bind_secure_boot) const;

 private:
  std::string binary_;
  std::string key_;
  bool allow_null_;
};

}  // namespace lhc
