#include "store/systemd_creds.h"

#include <algorithm>

#include "store/process.h"

namespace lhc {

namespace {

constexpr int kTimeoutMs = 30000;

// First line of what the tool printed, for the error detail.
std::string firstLine(std::string text) {
  text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
  const size_t eol = text.find('\n');
  return eol == std::string::npos ? text : text.substr(0, eol);
}

bool run(const std::string& binary, const std::vector<std::string>& args, const std::string& input,
         std::string& output, std::string& error) {
  const ProcessResult r = runProcess(binary, args, input, kTimeoutMs);
  if (!r.started || r.exit_code != 0) {
    error = r.started ? "systemd-creds failed: " + firstLine(r.err) : "cannot run " + binary;
    return false;
  }
  output = r.out;
  return true;
}

}  // namespace

std::vector<std::string> SystemdCreds::encryptArgs(const std::string& name,
                                                   bool bind_secure_boot) const {
  std::vector<std::string> args = {"encrypt", "--name=" + name, "--with-key=" + key_};
  if (bind_secure_boot) {
    args.push_back("--tpm2-pcrs=7");
  }
  args.push_back("-");
  args.push_back("-");
  return args;
}

bool SystemdCreds::encrypt(const std::string& name, const std::string& plain, bool bind_secure_boot,
                           std::string& cred, std::string& error) {
  return run(binary_, encryptArgs(name, bind_secure_boot), plain, cred, error);
}

bool SystemdCreds::decrypt(const std::string& name, const std::string& cred, std::string& plain,
                           std::string& error) {
  std::vector<std::string> args = {"decrypt", "--name=" + name};
  if (allow_null_) {
    args.push_back("--allow-null");
  }
  args.push_back("-");
  args.push_back("-");
  return run(binary_, args, cred, plain, error);
}

}  // namespace lhc
