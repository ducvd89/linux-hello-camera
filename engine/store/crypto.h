#pragma once

#include <string>

namespace lhc {

// Encrypts and decrypts the template of one user, with the TPM. `name` ties a credential to its
// user so files cannot be swapped between users. Behind an interface so tests need no TPM.
class Crypto {
 public:
  virtual ~Crypto() = default;

  // `bind_secure_boot`: also seal to the Secure Boot state (PCR 7), so the file only opens while
  // the same Secure Boot configuration is in place.
  virtual bool encrypt(const std::string& name, const std::string& plain, bool bind_secure_boot,
                       std::string& cred, std::string& error) = 0;
  virtual bool decrypt(const std::string& name, const std::string& cred, std::string& plain,
                       std::string& error) = 0;
};

// The name used for <user>'s credential.
inline std::string credentialName(const std::string& username) {
  return "linux-hello-camera-" + username;
}

}  // namespace lhc
