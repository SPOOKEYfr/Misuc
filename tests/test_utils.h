#pragma once

#include <cstdio>
#include <functional>
#include <vector>

namespace misuc::test {

inline int& checkCount() { static int n = 0; return n; }
inline int& failCount() { static int n = 0; return n; }
inline std::vector<std::pair<const char*, std::function<void()>>>& registry() {
  static std::vector<std::pair<const char*, std::function<void()>>> r;
  return r;
}

struct Register {
  Register(const char* name, std::function<void()> fn) {
    registry().emplace_back(name, std::move(fn));
  }
};

inline void report(bool ok, const char* expr, const char* file, int line) {
  ++checkCount();
  if (!ok) {
    ++failCount();
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
  }
}

} // namespace misuc::test

#define TEST_ASSERT(expr) \
  misuc::test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define TEST_CASE(name)                                                    \
  static void test_##name();                                               \
  static const misuc::test::Register reg_##name(#name, test_##name);       \
  static void test_##name()
