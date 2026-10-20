#pragma once

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <misuc/engine/audio/AudioBackend.h>
#if defined(_WIN32)
#include <misuc/engine/audio/WASAPIBackend.h>
#else
#include <misuc/engine/audio/PipeWireBackend.h>
#endif

namespace misuc::test {

// Platform-neutral backend builder so test files stay portable: they exercise
// the engine, not a specific device backend.
inline std::unique_ptr<misuc::engine::AudioBackend> defaultAudioBackend() {
#if defined(_WIN32)
  return std::make_unique<misuc::engine::WASAPIBackend>();
#else
  return std::make_unique<misuc::engine::PipeWireBackend>();
#endif
}

inline long long processId() {
#if defined(_WIN32)
  return static_cast<long long>(::_getpid());
#else
  return static_cast<long long>(::getpid());
#endif
}

// Temp-dir factory with no POSIX-only symbol: mkdtemp on POSIX, an LCG +
// _mkdir loop with the CRT on Windows. Returns "" when nothing could be made.
inline std::string makeTempDir(const std::string& prefix) {
#if defined(_WIN32)
  const char* tempRoot = std::getenv("TEMP");
  const std::string root = tempRoot && *tempRoot ? std::string(tempRoot)
                                                 : std::string(".");
  unsigned int seed = static_cast<unsigned int>(1717u + processId());
  for (int attempt = 0; attempt < 64; ++attempt) {
    seed = seed * 1664525u + 1013904223u;
    const std::string path =
        root + "\\" + prefix + std::to_string((seed >> 16) % 999983u);
    if (::_mkdir(path.c_str()) == 0)
      return path;
  }
  return std::string();
#else
  const std::string tmpl = "/tmp/" + prefix + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
#endif
}

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
