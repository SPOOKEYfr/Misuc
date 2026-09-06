#include <misuc/engine/core/Logger.h>

#include <cstdarg>
#include <ctime>
#include <thread>

#include <misuc/engine/core/RingBuffer.h>

namespace misuc::core {

struct Logger::Impl {
  using Queue = SPSCRing<LogRecord, 4096>;
  Queue queue;
  std::thread thread;
};

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

Logger::Logger() : impl_(new Impl) {
  impl_->thread = std::thread([this] { drain(); });
}

Logger::~Logger() {
  shutdown();
  delete impl_;
}

void Logger::shutdown() {
  bool expected = true;
  if (running_.compare_exchange_strong(expected, false)) {
    if (impl_->thread.joinable())
      impl_->thread.join();
  }
}

void Logger::log(LogLevel level, const char* fmt, ...) noexcept {
  if (static_cast<int>(level) < static_cast<int>(level_.load()))
    return;
  char buf[kLogLineMax];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  buf[sizeof buf - 1] = '\0';

  LogRecord rec;
  rec.timeUs = static_cast<std::uint64_t>(::time(nullptr));
  rec.level = static_cast<int>(level);
  std::strncpy(rec.text, buf, sizeof rec.text - 1);
  rec.text[sizeof rec.text - 1] = '\0';
  impl_->queue.push(rec);
}

bool Logger::pushRt(const char* text, LogLevel level) noexcept {
  if (static_cast<int>(level) < static_cast<int>(level_.load()))
    return true;
  LogRecord rec;
  rec.timeUs = static_cast<std::uint64_t>(::time(nullptr));
  rec.level = static_cast<int>(level);
  std::strncpy(rec.text, text, sizeof rec.text - 1);
  rec.text[sizeof rec.text - 1] = '\0';
  return impl_->queue.push(rec);
}

void Logger::drain() {
  static const char* kNames[] = {"DBG", "INF", "WRN", "ERR"};
  while (running_.load()) {
    LogRecord rec;
    bool got = false;
    for (int i = 0; i < 128 && (got = impl_->queue.pop(rec)); ++i) {
      std::fprintf(stderr, "[%s] %llu %s\n", kNames[rec.level],
                   static_cast<unsigned long long>(rec.timeUs), rec.text);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  LogRecord rec;
  while (impl_->queue.pop(rec)) {
    std::fprintf(stderr, "[%s] %llu %s\n", kNames[rec.level],
                 static_cast<unsigned long long>(rec.timeUs), rec.text);
  }
}

} // namespace misuc::core