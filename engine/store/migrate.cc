#include "store/migrate.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "face/enrolled_faces.h"

namespace lhc {

namespace {

bool isDirectory(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string stemOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  const std::string name = path.substr(slash + 1);
  return name.substr(0, name.rfind('.'));
}

bool allDigits(const std::string& s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Crops from 0.8 are named <unix-ms>.png, which makes a stable entry id.
TemplateEntry entryFor(const std::string& crop, std::vector<float> embedding, size_t index) {
  TemplateEntry e;
  const std::string stem = stemOf(crop);
  if (allDigits(stem) && stem.size() <= 16) {
    e.id = stem;
    e.created = std::atoll(stem.c_str()) / 1000;
  } else {
    e.id = "migrated-" + std::to_string(index);
    e.created = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
  }
  e.embedding = std::move(embedding);
  return e;
}

}  // namespace

bool shredFile(const std::string& path) {
  const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return false;
  }
  struct stat st;
  bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
  if (ok) {
    const std::vector<char> zeros(64 * 1024, 0);
    off_t left = st.st_size;
    while (ok && left > 0) {
      const size_t chunk = static_cast<size_t>(std::min<off_t>(left, zeros.size()));
      const ssize_t w = write(fd, zeros.data(), chunk);
      if (w <= 0) {
        ok = false;
      } else {
        left -= w;
      }
    }
    ok = ok && fsync(fd) == 0;
  }
  close(fd);
  // Unlink even if overwriting failed: the image must not stay readable under its name.
  return unlink(path.c_str()) == 0 && ok;
}

MigrateResult migrateFaces(const std::string& faces_root, TemplateStore& store,
                           const std::string& model, Storage target, const Embedder& embed) {
  MigrateResult result;
  DIR* dir = opendir(faces_root.c_str());
  if (dir == nullptr) {
    return result;
  }
  std::vector<std::string> users;
  while (const dirent* entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name[0] != '.' && isDirectory(faces_root + "/" + name)) {
      users.push_back(name);
    }
  }
  closedir(dir);
  std::sort(users.begin(), users.end());

  for (const std::string& user : users) {
    const std::string user_dir = faces_root + "/" + user;
    const std::vector<std::string> crops = listFaces(user_dir);

    std::vector<TemplateEntry> entries;
    for (size_t i = 0; i < crops.size(); i++) {
      const ImageRGB crop = readImage(crops[i]);
      if (crop.empty()) {
        continue;
      }
      std::vector<float> embedding = embed(crop);
      if (!embedding.empty()) {
        entries.push_back(entryFor(crops[i], std::move(embedding), i));
      }
    }
    if (entries.empty()) {
      if (!crops.empty()) {
        result.failed.emplace_back(user, "no usable face in the crops");
      }
      continue;
    }

    LoadResult existing = store.load(user, model, target);
    Template tmpl;
    if (existing.status == LoadStatus::kOk) {
      tmpl = std::move(existing.tmpl);
    } else {
      // Nothing to keep: no template yet, or one the configured model can't use.
      tmpl.model = model;
      tmpl.dim = static_cast<int>(entries.front().embedding.size());
    }
    for (TemplateEntry& e : entries) {
      const bool known = std::any_of(tmpl.entries.begin(), tmpl.entries.end(),
                                     [&](const TemplateEntry& t) { return t.id == e.id; });
      if (!known && static_cast<int>(e.embedding.size()) == tmpl.dim) {
        tmpl.entries.push_back(std::move(e));
      }
    }

    std::string error;
    if (!store.save(user, tmpl, target, error)) {
      result.failed.emplace_back(user, error);
      continue;
    }
    // The template is safely written; now the pictures go.
    for (const std::string& crop : crops) {
      shredFile(crop);
    }
    rmdir(user_dir.c_str());
    result.migrated.push_back(user);
  }
  rmdir(faces_root.c_str());  // only succeeds when empty
  return result;
}

}  // namespace lhc
