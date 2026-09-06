#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace misuc::core {

// Real-time-safe logger. The real-time thread only pushes a fixed-size record
// into a lock-free queue; a dedicated thread drains it to the console.
constexpr std::size_t kLogLineMax = 192;

struct LogRecord {
  std::uint64_t timeUs = 0;
  int level = 0; // 0=debug 1=info 2=warn 3=error
  char text[kLogLineMax];
};

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Global logger with a background drain thread. Safe to call from any thread.
class Logger {
public:
  static Logger& instance();

  void setLevel(LogLevel level) noexcept { level_.store(level); }
  void log(LogLevel level, const char* fmt, ...) noexcept;

  // Real-time safe: returns false if the queue is full (record dropped).
  bool pushRt(const char* text, LogLevel level) noexcept;

  void shutdown();

private:
  Logger();
  ~Logger();
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  void drain();

  std::atomic<LogLevel> level_{LogLevel::Debug};
  std::atomic<bool> running_{true};
  struct Impl;
  Impl* impl_;
};

template <typename... Args>
void logInfo(const char* fmt, Args&&... args) noexcept {
  Logger::instance().log(LogLevel::Info, fmt, std::forward<Args>(args)...);
}

} // namespace misuc::core