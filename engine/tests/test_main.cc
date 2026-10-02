#include <cstdio>

#include "tests/test.h"

int main() {
  testCamera();
  testConfig();
  testLiveness();
  testMatching();
  testStorage();
  testDaemon();
  const auto& r = lhc_test::registry();
  std::printf("%d checks, %d failures\n", r.checks, r.failures);
  return r.failures == 0 ? 0 : 1;
}
