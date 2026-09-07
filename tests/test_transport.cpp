#include "test_utils.h"

#include <cmath>
#include <string>

#include <misuc/engine/transport/Transport.h>

using misuc::engine::Transport;

TEST_CASE(transport_position_advance) {
  Transport t;
  t.setSampleRate(48000);
  t.setTempo(120); // 1 spb = 24000 frames
  t.play();
  TEST_ASSERT(t.advance(24000) == 24000);
  int bar = 0, beat = 0;
  double frac = 0;
  t.barsBeats(t.positionFrames(), bar, beat, frac);
  TEST_ASSERT(bar == 1);     // one beat elapsed, still bar 1
  TEST_ASSERT(beat == 2);    // on beat 2 of bar 1
  TEST_ASSERT(frac == 0.0);
}

TEST_CASE(transport_pause_advance_zero) {
  Transport t;
  t.setSampleRate(48000);
  t.pause();
  TEST_ASSERT(t.advance(48000) == 0);
}

TEST_CASE(transport_loop_wrap) {
  Transport t;
  t.setSampleRate(48000);
  t.setLooping(true);
  t.setLoopRange(48000, 96000); // bar 2..3
  t.setPosition(48000);
  t.play();
  t.advance(48000); // wraps to loopStart = 48000
  TEST_ASSERT(t.positionFrames() == 48000);
}

TEST_CASE(transport_loop_disabled_does_not_wrap) {
  Transport t;
  t.setSampleRate(48000);
  t.setLooping(false);
  t.setLoopRange(48000, 96000);
  t.setPosition(96000);
  t.play();
  t.advance(48000); // would wrap if enabled
  TEST_ASSERT(t.positionFrames() == 144000);
}

TEST_CASE(transport_format_position) {
  Transport t;
  t.setSampleRate(48000);
  t.setTempo(120); // spb = 24000, bar = 96000
  char buf[32];
  t.formatPosition(0, buf, sizeof buf);
  TEST_ASSERT(std::string(buf) == "1.1.1.000");
  t.formatPosition(24000, buf, sizeof buf); // one beat in
  TEST_ASSERT(std::string(buf) == "1.2.1.000");
  t.formatPosition(96000, buf, sizeof buf); // one bar
  TEST_ASSERT(std::string(buf) == "2.1.1.000");
}

TEST_CASE(transport_tempo_changes_affect_math) {
  Transport t;
  t.setSampleRate(48000);
  t.setTempo(240); // spb = 12000
  TEST_ASSERT(std::fabs(t.framesPerBeat() - 12000.0) < 1.0);
  t.setTempo(9999);
  TEST_ASSERT(t.tempo() <= 400.0);
  t.setTempo(1);
  TEST_ASSERT(t.tempo() >= 20.0);
}