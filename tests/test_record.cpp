// Item #4: record-playthrough integration. Record a take while the transport
// plays a loop through the engine; the input must be mixed into the output
// (input monitoring / playthrough) at the same time it is captured into the
// take, so the session can build a clip from the captured input that matches
// exactly what the performer heard. Renders headless by driving
// AudioEngine::process() directly (no daemon needed).

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/dsp/Midi.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "test_utils.h"

using misuc::engine::AudioEngine;

namespace {

constexpr uint32_t kSr = 48000u;
constexpr uint32_t kFpb120 = 24000u; // 120 bpm => 0.5 s per beat
constexpr double kNoteHz = 261.6256; // MIDI 60
constexpr double kInputHz = 523.25;  // C5, distinct from the loop's note

// A deterministic 0.25-amplitude sine fed to the engine as mono input.
std::vector<float> makeInput(uint32_t frames) {
  std::vector<float> in(frames);
  for (uint32_t i = 0; i < frames; ++i)
    in[i] = 0.25f * static_cast<float>(std::sin(
                       2.0 * std::acos(-1.0) * kInputHz * static_cast<double>(i) /
                       kSr));
  return in;
}

float peakIn(const std::vector<float>& v, size_t from, size_t to) {
  float p = 0.f;
  for (size_t i = from; i < to && i < v.size(); ++i)
    p = std::max(p, std::fabs(v[i]));
  return p;
}

} // namespace

TEST_CASE(record_captures_input_while_loop_plays) {
  constexpr uint32_t kFrames = 4 * kFpb120; // beats 0..4

  // Baseline render: the loop alone (no input), so we can prove the
  // playthrough input is actually printed into the monitored output.
  AudioEngine base(std::make_unique<misuc::engine::PipeWireBackend>());
  base.setGroove(false);
  base.setMetronome(false, 0.0f);
  base.setTempo(120.0);
  base.setScore(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, kNoteHz, 1.f, 0}}); // clip/notes on beats 0..2
  base.play(false);
  std::vector<float> baseOut(kFrames * 2, 0.f);
  base.process(nullptr, 0, baseOut.data(), 2, kFrames, 0, kSr);
  const float loopPeak = peakIn(baseOut, 8000, 40000); // beats 0..2
  TEST_ASSERT(loopPeak > 0.05f); // loop is really playing

  // The real scenario: arm record, start the transport, feed the input.
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0);
  engine.setScore(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, kNoteHz, 1.f, 0}});
  engine.setMonitorGainDb(0.f); // playthrough is an explicit, real feature
  engine.play(false);
  engine.startRecord();

  const std::vector<float> in = makeInput(kFrames);
  std::vector<float> out(kFrames * 2, 0.f);
  engine.process(in.data(), 1, out.data(), 2, kFrames, 0, kSr);
  engine.stopRecord();

  // Playthrough: the monitored input must be audible in the output where the
  // loop is playing (loop + input, louder than the loop alone) and appear on
  // its own where the loop has finished but the transport runs on.
  const float whileLoop = peakIn(out, 8000, 40000);
  const float pastLoop = peakIn(out, 56000, 88000);
  TEST_ASSERT(pastLoop > 0.15f);                  // input printing alone
  TEST_ASSERT(whileLoop > loopPeak);              // loop + input
  TEST_ASSERT(whileLoop > pastLoop);              // and the loop adds energy

  // Drain the stop-record control (offline there is no stream to do it).
  std::vector<float> drainBuf(2400 * 2, 0.f);
  engine.process(nullptr, 0, drainBuf.data(), 2, 2400, kFrames, kSr);

  // The take must contain exactly the input that was played through.
  std::vector<float> take;
  uint32_t takeSr = 0, takeCh = 0;
  TEST_ASSERT(engine.takeRecordedSamples(take, takeSr, takeCh));
  TEST_ASSERT(takeCh == 1);
  TEST_ASSERT(takeSr == kSr);
  TEST_ASSERT(take.size() == kFrames);
  float maxErr = 0.f;
  for (uint32_t i = 0; i < kFrames; ++i)
    maxErr = std::max(maxErr, std::fabs(take[i] - in[i]));
  TEST_ASSERT(maxErr < 1e-4f);
  // The take is the input, not the loop: it is silent only where the input is
  // silent (none here) and carries the sine, whereas a bounce of the loop
  // would be silent past beat 2.
  TEST_ASSERT(peakIn(take, 56000, 88000) > 0.15f);
}

// Recording while stopped must capture nothing (matches the transport-gated
// arm behavior the UI relies on for count-in recording).
TEST_CASE(record_stopped_captures_nothing) {
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0);
  engine.startRecord(); // not playing
  const std::vector<float> in = makeInput(kFpb120);
  std::vector<float> out(kFpb120 * 2, 0.f);
  engine.process(in.data(), 1, out.data(), 2, kFpb120, 0, kSr);
  engine.stopRecord();
  std::vector<float> drainBuf(2400 * 2, 0.f);
  engine.process(nullptr, 0, drainBuf.data(), 2, 2400, kFpb120, kSr);
  std::vector<float> take;
  uint32_t sr = 0, ch = 0;
  TEST_ASSERT(!engine.takeRecordedSamples(take, sr, ch));
  TEST_ASSERT(take.empty());
}