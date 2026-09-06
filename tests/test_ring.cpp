#include "test_utils.h"

#include <misuc/engine/core/RingBuffer.h>

using misuc::core::SPSCRing;

TEST_CASE(spsc_basic) {
  SPSCRing<int, 8> ring;
  for (int i = 0; i < 8; ++i)
    TEST_ASSERT(ring.push(i));
  // full now
  TEST_ASSERT(!ring.push(99));
  for (int i = 0; i < 8; ++i) {
    int v = -1;
    TEST_ASSERT(ring.pop(v));
    TEST_ASSERT(v == i);
  }
  int v = -1;
  TEST_ASSERT(!ring.pop(v));
  TEST_ASSERT(v == -1);
}

TEST_CASE(spsc_wrap) {
  SPSCRing<int, 16> ring;
  for (int turn = 0; turn < 200; ++turn) {
    for (int i = 0; i < 3; ++i)
      TEST_ASSERT(ring.push(turn * 10 + i));
    for (int i = 0; i < 3; ++i) {
      int v = -1;
      TEST_ASSERT(ring.pop(v));
      TEST_ASSERT(v == turn * 10 + i);
    }
  }
}