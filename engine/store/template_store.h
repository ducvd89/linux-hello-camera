#pragma once

#include <string>
#include <vector>

#include "core/hardware.h"
#include "store/crypto.h"
#include "store/template.h"

namespace lhc {

// How a user's template is kept on disk:
//   none  = <user>.json         plain embeddings (still root-only)
//   tpm   = <user>.tpm.cred     systemd-creds, host+tpm2
//   tpm-sb = <user>.tpm-sb.cred as tpm, also sealed to Secure Boot (PCR 7)
enum class Storage { kNone, kTpm, kTpmSb };

inline constexpr Storage kAllStorages[] = {Storage::kNone, Storage::kTpm, Storage::kTpmSb};

const char* storageName(Storage storage);  // "none" | "tpm" | "tpm-sb"

// Which form templates take: TPM when the setting asks for it and the PC has one (see
// tpmEncryptionEffective), and then sealed to Secure Boot if that is enabled.
Storage chooseStorage(TpmSetting setting, const Hardware& hardware);

enum class LoadStatus { kOk, kNotEnrolled, kNeedsReenrol };

struct LoadResult {
  LoadStatus status = LoadStatus::kNotEnrolled;
  Template tmpl;                     // valid when status == kOk
  Storage storage = Storage::kNone;  // which file it came from
  std::string detail;                // why, for kNeedsReenrol
};

enum class ConvertStatus { kUnchanged, kConverted, kFailed };

// Face templates under one directory (root:root 0700, files 0600). Not thread safe; callers
// serialise.
class TemplateStore {
 public:
  TemplateStore(std::string dir, Crypto& crypto) : dir_(std::move(dir)), crypto_(crypto) {}

  // Loads the user's template. A template made with a recognition model other than `model`, or one
  // that cannot be read or decrypted (TPM cleared, Secure Boot state changed), is kNeedsReenrol. If
  // several files exist (a conversion was interrupted) the one matching `prefer` wins.
  LoadResult load(const std::string& user, const std::string& model, Storage prefer) const;

  // Writes the template in `target` form (atomically) and removes the other forms' files.
  bool save(const std::string& user, const Template& tmpl, Storage target, std::string& error);

  // Moves the user's template to `target` without looking at its content. kFailed when it can't be
  // decrypted (it then stays as it is and the user must enrol again).
  ConvertStatus convert(const std::string& user, Storage target, std::string& error);

  // Deletes the entry; `found` tells whether it existed. Needs a readable template.
  bool removeEntry(const std::string& user, const std::string& id, const std::string& model,
                   Storage target, bool& found, std::string& error);

  // Deletes the user's template in either form.
  bool clear(const std::string& user, std::string& error);

  // Users that have a template file.
  std::vector<std::string> users() const;

 private:
  std::string pathFor(const std::string& user, Storage storage) const;
  bool readPlain(const std::string& user, Storage storage, std::string& plain,
                 std::string& error) const;
  bool ensureDir(std::string& error) const;
  void removeOthers(const std::string& user, Storage keep) const;

  std::string dir_;
  Crypto& crypto_;
};

}  // namespace lhc
