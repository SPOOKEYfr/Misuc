#include "test_utils.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

void runAll() {
  for (const auto& [name, fn] : misuc::test::registry()) {
    const int before = misuc::test::checkCount();
    std::printf("CASE %s\n", name);
    try {
      fn();
    } catch (const std::exception& e) {
      misuc::test::report(false, "unexpected exception", __FILE__, __LINE__);
      std::fprintf(stderr, "  EXCEPTION: %s\n", e.what());
    }
    (void)before;
  }
  const int failed = misuc::test::failCount();
  const int checks = misuc::test::checkCount();

  std::printf("\n%d checks, %d failures\n", checks, failed);
  // Grep-friendly summary for CI.
  std::printf("RESULT %s (%d checks, %d failures)\n",
              failed == 0 ? "PASS" : "FAIL", checks, failed);
  if (failed != 0) {
    std::fprintf(stderr, "FAILURES PRESENT: %d\n", failed);
    std::exit(1);
  }
}

int main() {
  runAll();
  return 0;
}