#include "store/template_store.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "core/file_util.h"

namespace lhc {

namespace {

bool fileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() > suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

namespace {

const char* suffixOf(Storage storage) {
  switch (storage) {
    case Storage::kTpm:
      return ".tpm.cred";
    case Storage::kTpmSb:
      return ".tpm-sb.cred";
    case Storage::kNone:
      break;
  }
  return ".json";
}

}  // namespace

const char* storageName(Storage storage) {
  switch (storage) {
    case Storage::kTpm:
      return "tpm";
    case Storage::kTpmSb:
      return "tpm-sb";
    case Storage::kNone:
      break;
  }
  return "none";
}

Storage chooseStorage(TpmSetting setting, const Hardware& hardware) {
  if (!tpmEncryptionEffective(setting, hardware)) {
    return Storage::kNone;
  }
  return hardware.secure_boot ? Storage::kTpmSb : Storage::kTpm;
}

std::string TemplateStore::pathFor(const std::string& user, Storage storage) const {
  return dir_ + "/" + user + suffixOf(storage);
}

bool TemplateStore::ensureDir(std::string& error) const {
  if (mkdir(dir_.c_str(), 0700) != 0 && errno != EEXIST) {
    error = std::string("mkdir ") + dir_ + ": " + std::strerror(errno);
    return false;
  }
  if (chmod(dir_.c_str(), 0700) != 0) {
    error = std::string("chmod ") + dir_ + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

bool TemplateStore::readPlain(const std::string& user, Storage storage, std::string& plain,
                              std::string& error) const {
  std::string raw;
  if (!readFile(pathFor(user, storage), raw)) {
    error = "cannot read template";
    return false;
  }
  if (storage == Storage::kNone) {
    plain = std::move(raw);
    return true;
  }
  // The credential carries its own key description (with or without PCR 7), so decrypting does
  // not need to know which kind it was.
  return crypto_.decrypt(credentialName(user), raw, plain, error);
}

LoadResult TemplateStore::load(const std::string& user, const std::string& model,
                               Storage prefer) const {
  LoadResult result;
  Storage found = prefer;
  if (!fileExists(pathFor(user, prefer))) {
    const auto it = std::find_if(std::begin(kAllStorages), std::end(kAllStorages),
                                 [&](Storage s) { return fileExists(pathFor(user, s)); });
    if (it == std::end(kAllStorages)) {
      return result;  // kNotEnrolled
    }
    found = *it;
  }
  result.storage = found;
  result.status = LoadStatus::kNeedsReenrol;

  std::string plain, error;
  if (!readPlain(user, found, plain, error)) {
    result.detail = error;
    return result;
  }
  Template tmpl;
  if (!templateFromJson(plain, tmpl, error)) {
    result.detail = error;
    return result;
  }
  if (tmpl.model != model) {
    result.detail = "template was made with " + tmpl.model;
    return result;
  }
  result.tmpl = std::move(tmpl);
  result.status = LoadStatus::kOk;
  return result;
}

bool TemplateStore::save(const std::string& user, const Template& tmpl, Storage target,
                         std::string& error) {
  if (!ensureDir(error)) {
    return false;
  }
  const std::string json = templateToJson(tmpl);
  std::string content;
  if (target != Storage::kNone) {
    if (!crypto_.encrypt(credentialName(user), json, target == Storage::kTpmSb, content, error)) {
      return false;
    }
  } else {
    content = json;
  }
  if (!atomicWriteFile(pathFor(user, target), content, 0600, error)) {
    return false;
  }
  // Only after the new file is safely there do the old forms go.
  removeOthers(user, target);
  return true;
}

void TemplateStore::removeOthers(const std::string& user, Storage keep) const {
  for (Storage s : kAllStorages) {
    if (s != keep) {
      unlink(pathFor(user, s).c_str());
    }
  }
}

ConvertStatus TemplateStore::convert(const std::string& user, Storage target, std::string& error) {
  std::vector<Storage> others;
  for (Storage s : kAllStorages) {
    if (s != target && fileExists(pathFor(user, s))) {
      others.push_back(s);
    }
  }
  if (others.empty()) {
    return ConvertStatus::kUnchanged;
  }
  if (fileExists(pathFor(user, target))) {
    // An interrupted conversion left both; the new file is written before the old goes, so it is
    // complete.
    removeOthers(user, target);
    return ConvertStatus::kConverted;
  }

  std::string plain;
  if (!readPlain(user, others.front(), plain, error)) {
    return ConvertStatus::kFailed;
  }
  Template tmpl;
  if (!templateFromJson(plain, tmpl, error)) {
    return ConvertStatus::kFailed;
  }
  return save(user, tmpl, target, error) ? ConvertStatus::kConverted : ConvertStatus::kFailed;
}

bool TemplateStore::removeEntry(const std::string& user, const std::string& id,
                                const std::string& model, Storage target, bool& found,
                                std::string& error) {
  found = false;
  LoadResult loaded = load(user, model, target);
  if (loaded.status != LoadStatus::kOk) {
    error = loaded.status == LoadStatus::kNotEnrolled ? "not enrolled" : "needs re-enrolment";
    return false;
  }
  Template& t = loaded.tmpl;
  const auto it = std::remove_if(t.entries.begin(), t.entries.end(),
                                 [&](const TemplateEntry& e) { return e.id == id; });
  if (it == t.entries.end()) {
    return true;
  }
  found = true;
  t.entries.erase(it, t.entries.end());
  if (t.entries.empty()) {
    return clear(user, error);
  }
  return save(user, t, target, error);
}

bool TemplateStore::clear(const std::string& user, std::string& error) {
  for (Storage s : kAllStorages) {
    const std::string path = pathFor(user, s);
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
      error = "cannot delete " + path + ": " + std::strerror(errno);
      return false;
    }
  }
  return true;
}

std::vector<std::string> TemplateStore::users() const {
  std::vector<std::string> users;
  DIR* dir = opendir(dir_.c_str());
  if (dir == nullptr) {
    return users;
  }
  while (const dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name[0] == '.') {
      continue;
    }
    for (Storage s : kAllStorages) {
      const std::string suffix = suffixOf(s);
      if (endsWith(name, suffix)) {
        const std::string user = name.substr(0, name.size() - suffix.size());
        if (std::find(users.begin(), users.end(), user) == users.end()) {
          users.push_back(user);
        }
        break;
      }
    }
  }
  closedir(dir);
  std::sort(users.begin(), users.end());
  return users;
}

}  // namespace lhc
