#include "test_utils.h"

#include <misuc/engine/core/Json.h>
#include <misuc/engine/core/Logger.h>
#include <misuc/engine/core/RingBuffer.h>

using namespace misuc::test;

TEST_CASE(logger_accepts_messages) {
  misuc::core::Logger& logger = misuc::core::Logger::instance();
  logger.setLevel(misuc::core::LogLevel::Debug);
  logger.pushRt("test from core", misuc::core::LogLevel::Info);
  logger.log(misuc::core::LogLevel::Info, "formatted %d %s", 42, "ok");
  logger.setLevel(misuc::core::LogLevel::Info);
}