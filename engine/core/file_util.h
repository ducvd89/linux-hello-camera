#pragma once

#include <string>

namespace lhc {

// Writes `content` to `path` so a reader (or a crash) sees either the old file or the whole new
// one: a temp file in the same directory, fsync, rename, fsync of the directory. The file ends up
// with `mode`.
bool atomicWriteFile(const std::string& path, const std::string& content, unsigned mode,
                     std::string& error);

// Reads a whole file; false if it can't be opened.
bool readFile(const std::string& path, std::string& out);

}  // namespace lhc
