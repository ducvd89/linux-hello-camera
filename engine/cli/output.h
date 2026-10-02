#pragma once

#include <iostream>
#include <string>

namespace lhc {

// A JSON string literal for `text`.
std::string jsonString(const std::string& text);

// Writes one line to stdout and flushes, so a parent reading a pipe sees it at once.
inline void printLine(const std::string& line) { std::cout << line << std::endl; }

}  // namespace lhc
