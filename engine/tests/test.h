#pragma once

// A deliberately tiny test harness: CHECK records a failure and carries on, main() prints the
// total and sets the exit code.

#include <cmath>
#include <cstdio>

namespace lhc_test {

struct Registry {
  int checks = 0;
  int failures = 0;
};

inline Registry& registry() {
  static Registry r;
  return r;
}

}  // namespace lhc_test

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++lhc_test::registry().checks;                                                  \
    if (!(cond)) {                                                                  \
      ++lhc_test::registry().failures;                                              \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                               \
  } while (0)

#define CHECK_NEAR(a, b, eps) \
  CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (eps))

void testCamera();
void testConfig();
void testLiveness();
void testMatching();
void testStorage();
void testDaemon();
