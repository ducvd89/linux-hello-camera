#pragma once

#include <string>

#include "store/crypto.h"

namespace lhc_test {

// A reversible stand-in for systemd-creds: the "credential" is the plaintext behind a header that
// carries the name and whether Secure Boot was bound. Decrypting with another name fails, as the
// real one does.
class FakeCrypto : public lhc::Crypto {
 public:
  bool fail_decrypt = false;  // pretend the TPM was cleared / PCR 7 changed
  bool fail_encrypt = false;
  int encrypts = 0;
  bool last_bound_secure_boot = false;

  bool encrypt(const std::string& name, const std::string& plain, bool bind_secure_boot,
               std::string& cred, std::string& error) override {
    if (fail_encrypt) {
      error = "fake encrypt failure";
      return false;
    }
    encrypts++;
    last_bound_secure_boot = bind_secure_boot;
    cred = std::string("FAKE:") + (bind_secure_boot ? "sb" : "nosb") + ":" + name + ":" + plain;
    return true;
  }

  bool decrypt(const std::string& name, const std::string& cred, std::string& plain,
               std::string& error) override {
    const std::string prefix_sb = "FAKE:sb:" + name + ":";
    const std::string prefix = "FAKE:nosb:" + name + ":";
    if (fail_decrypt) {
      error = "fake decrypt failure";
      return false;
    }
    for (const std::string& p : {prefix_sb, prefix}) {
      if (cred.compare(0, p.size(), p) == 0) {
        plain = cred.substr(p.size());
        return true;
      }
    }
    error = "fake: wrong name or not a credential";
    return false;
  }
};

}  // namespace lhc_test
