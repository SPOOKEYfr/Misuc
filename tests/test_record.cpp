// Phase G recording: record-playthrough integration. Record a take while the
// transport plays a loop through the engine; the input must be mixed into the
// output (input monitoring / playthrough) at the same time it is captured into
// the take, so the session can build a clip from the captured input that
// matches exactly what the performer heard. The take also carries its exact
// transport span (start/end beats), which survives metronome count-in (the
// engine never records the clicks) and loop wrap (a take is one continuous
// tape across loop passes). Renders headless by driving AudioEngine::process()
// directly (no daemon needed).

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/dsp/Midi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "test_utils.h"

using misuc::engine::AudioEngine;

namespace {

constexpr uint32_t kSr = 48000u;
constexpr uint32_t kFpb120 = 24000u; // 120 bpm => 0.5 s per beat
constexpr double kNoteHz = 261.6256; // MIDI 60
constexpr double kInputHz = 659.25;  // E5, well clear of the loop note so the
                                     // sum actually rises above either alone

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

// A fresh engine with the standard loop (beats 0..2) and monitoring on.
void setupEngine(AudioEngine& engine) {
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0);
  engine.setScore(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, kNoteHz, 1.f, 0}}); // clip/notes on beats 0..2
  engine.setMonitorGainDb(0.f);       // playthrough is an explicit, real feature
}

std::unique_ptr<AudioEngine> makeEngine() {
  auto e = std::make_unique<AudioEngine>(
      misuc::test::defaultAudioBackend());
  setupEngine(*e);
  return e;
}

// Drive process() in per-beat audio blocks (the way a real stream delivers
// frames), feeding a mono input and collecting the stereo output.
void runBlocks(AudioEngine& engine, const std::vector<float>& in,
               std::vector<float>* out) {
  const uint32_t total = static_cast<uint32_t>(in.size());
  if (out)
    out->assign(total * 2, 0.f);
  uint32_t pos = 0;
  uint32_t block = 0;
  while (pos < total) {
    const uint32_t n = std::min<uint32_t>(kFpb120, total - pos);
    std::vector<float> outc(n * 2, 0.f);
    engine.process(in.data() + pos, 1, outc.data(), 2, n, block * kFpb120, kSr);
    if (out) {
      for (uint32_t i = 0; i < n * 2; ++i)
        (*out)[pos * 2 + i] = outc[i];
    }
    pos += n;
    ++block;
  }
}

// Drain the stop-record control the way the offline path must (with a live
// stream stopRecord waits for the audio thread instead).
void drainStopControls(AudioEngine& engine, uint64_t songPos) {
  std::vector<float> drainBuf(2400 * 2, 0.f);
  engine.process(nullptr, 0, drainBuf.data(), 2, 2400, songPos, kSr);
}

} // namespace

TEST_CASE(record_captures_input_while_loop_plays) {
  constexpr uint32_t kFrames = 4 * kFpb120; // beats 0..4

  // Baseline render: the loop alone (no input), so we can prove the
  // playthrough input is actually printed into the monitored output.
  auto basePtr = makeEngine(); AudioEngine& base = *basePtr;
  base.play(false);
  std::vector<float> baseOut;
  runBlocks(base, std::vector<float>(kFrames, 0.f), &baseOut);
  const float loopPeak = peakIn(baseOut, 8000, 40000); // beats 0..2
  TEST_ASSERT(loopPeak > 0.05f); // loop is really playing

  // The real scenario: arm record, start the transport, feed the input.
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.play(false);
  engine.startRecord();

  const std::vector<float> in = makeInput(kFrames);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, kFrames);

  // Playthrough: the monitored input must be audible in the output where the
  // loop is playing (loop + input, louder than the loop alone) and appear on
  // its own where the loop has finished (beats 2..4) but the transport runs on.
  const float whileLoop = peakIn(out, 8000, 40000);        // beats 0.08..0.42
  const float pastLoop = peakIn(out, 112000, 144000);      // beats 2.33..3.0
  TEST_ASSERT(pastLoop > 0.15f);                  // input printing alone
  TEST_ASSERT(whileLoop > loopPeak);              // loop + input
  TEST_ASSERT(whileLoop > pastLoop);              // and the loop adds energy

  // The take must contain exactly the input that was played through.
  misuc::engine::RecordCapture cap;
  TEST_ASSERT(engine.takeRecord(cap));
  TEST_ASSERT(cap.channels == 1);
  TEST_ASSERT(cap.sampleRate == kSr);
  TEST_ASSERT(cap.samples.size() == kFrames);
  float maxErr = 0.f;
  for (uint32_t i = 0; i < kFrames; ++i)
    maxErr = std::max(maxErr, std::fabs(cap.samples[i] - in[i]));
  TEST_ASSERT(maxErr < 1e-4f);
  // The take is the input, not the loop: it is silent only where the input is
  // silent (none here) and carries the sine, whereas a bounce of the loop
  // would be silent past beat 2.
  TEST_ASSERT(peakIn(cap.samples, 56000, 88000) > 0.15f);
  // The take carries its exact transport span: beats 0..4 as played.
  TEST_ASSERT(std::fabs(cap.startBeats - 0.0) < 0.02);
  TEST_ASSERT(std::fabs(cap.endBeats - 4.0) < 0.02);
  TEST_ASSERT(cap.framesLostTail == 0);
}

// Recording while stopped must capture nothing (matches the transport-gated
// arm behavior the UI relies on for count-in recording).
TEST_CASE(record_stopped_captures_nothing) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.startRecord(); // not playing
  const std::vector<float> in = makeInput(kFpb120);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, kFpb120);
  misuc::engine::RecordCapture cap;
  TEST_ASSERT(!engine.takeRecord(cap));
  TEST_ASSERT(cap.samples.empty());
}

// A metronome count-in leads the transport before any capture: arming while
// stopped and starting with play(true) must capture exactly the bar after the
// clicks and report its span as beats 0..2 — the click beats are excluded and
// the clip lands where the performance actually started.
TEST_CASE(record_countin_excludes_click_beats) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.setMetronome(true, 4.0f); // one 4/4 bar of clicks (beats 0..3.99)
  engine.play(true);               // count-in pending
  engine.startRecord();

  constexpr uint32_t kCountFrames = 4 * kFpb120; // the click bar
  constexpr uint32_t kRecFrames = 2 * kFpb120;   // beats 0..2 of the perf
  const std::vector<float> in = makeInput(kCountFrames + kRecFrames);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, kCountFrames + kRecFrames);

  misuc::engine::RecordCapture cap;
  TEST_ASSERT(engine.takeRecord(cap));
  TEST_ASSERT(cap.samples.size() == kRecFrames);  // clicks were not recorded
  TEST_ASSERT(std::fabs(cap.startBeats - 0.0) < 0.02); // perf starts at 0
  TEST_ASSERT(std::fabs(cap.endBeats - 2.0) < 0.02);
  TEST_ASSERT(cap.framesLostTail == 0);
  // The captured audio is the tail of the input (the performance), not the
  // start (which would be the click window).
  float tailErr = 0.f;
  for (uint32_t i = 0; i < kRecFrames; ++i)
    tailErr = std::max(tailErr,
                       std::fabs(cap.samples[i] - in[kCountFrames + i]));
  TEST_ASSERT(tailErr < 1e-4f);
}

// Recording across loop wrap stays one continuous tape: never split, never
// reset, and the span covers every pass.
TEST_CASE(record_across_loop_wrap_is_one_continuous_take) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.setLoop(true, 0.0, 2.0); // loop beats 0..2
  engine.play(false);
  engine.startRecord();

  constexpr uint32_t kLoopFrames = 2 * kFpb120;
  constexpr uint32_t kPasses = 3;
  const uint32_t total = kLoopFrames * kPasses;
  const std::vector<float> in = makeInput(total);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, total);

  misuc::engine::RecordCapture cap;
  TEST_ASSERT(engine.takeRecord(cap));
  TEST_ASSERT(cap.samples.size() == total); // every pass captured
  // The take started at beat 0; its end in transport coordinates is the loop
  // end (the playhead wrapped back to 0 after each pass). The accumulated
  // length is what a DAW must use for the clip: sample-derived, i.e. all 3
  // passes = 6 beats here.
  TEST_ASSERT(std::fabs(cap.startBeats - 0.0) < 0.02);
  TEST_ASSERT(std::fabs(cap.endBeats - 2.0) < 0.02);
  const double audioBeats =
      cap.samples.size() / static_cast<double>(cap.sampleRate) * 120.0 / 60.0;
  TEST_ASSERT(std::fabs(audioBeats - 6.0) < 0.02);
  TEST_ASSERT(cap.framesLostTail == 0);
  // One continuous tape: the sample at the loop-wrap instant (beat 2, where
  // the transport jumped) is the input continuing at that instant, so every
  // beat-sized window still carries the sine peak (a split would drop audio
  // there).
  TEST_ASSERT(peakIn(cap.samples, 2 * kFpb120 - 256, 2 * kFpb120 + 256) > 0.15f);
}

// Phase Lat: input latency compensation. When the capture stream reports an
// input delay (device + buffer), the take's span is advanced so the clip lands
// where the performance happened, not where the samples physically arrived.
TEST_CASE(record_input_latency_compensation_recenters_span) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;

  // Simulate a real device: 10 ms of input latency at 48 kHz = 480 frames.
  engine.setInputLatencyMsOverride(10.0);
  // Start the transport at beat 2 so the compensated start stays above 0
  // (the latch clamps at song start, which would hide part of the shift).
  engine.seekToBeat(2.0);
  engine.play(false);
  engine.startRecord();

  constexpr uint32_t kRecFrames = 2 * kFpb120; // beats 2..4 in transport coords
  const std::vector<float> in = makeInput(kRecFrames);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, 4 * kFpb120);

  misuc::engine::RecordCapture cap;
  TEST_ASSERT(engine.takeRecord(cap));
  // The offset was applied and reported.
  TEST_ASSERT(cap.captureOffsetFrames == 480);
  TEST_ASSERT(engine.captureOffsetFrames() == 480);
  // The take began 480 frames (2 beats × 24000 − 480) before the sample
  // arrival position: beat 1.98 instead of beat 2.0.
  TEST_ASSERT(std::fabs(cap.startBeats - (2.0 - 480.0 / kFpb120)) < 0.02);
  // And the last written sample is placed at the arrival position minus the
  // same offset.
  TEST_ASSERT(std::fabs(cap.endBeats -
                        (4.0 - 480.0 / kFpb120)) < 0.02);
  // The audio payload is unchanged: compensation moves the placement, never
  // the samples.
  TEST_ASSERT(cap.samples.size() == kRecFrames);
}

// A zero (or unset) input latency must leave the capture span untouched --
// the headless baseline every existing record test relies on.
TEST_CASE(record_zero_latency_keeps_baseline_span) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.setInputLatencyMsOverride(0.0);
  engine.play(false);
  engine.startRecord();
  const std::vector<float> in = makeInput(2 * kFpb120);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, 2 * kFpb120);
  misuc::engine::RecordCapture cap;
  TEST_ASSERT(engine.takeRecord(cap));
  TEST_ASSERT(cap.captureOffsetFrames == 0);
  TEST_ASSERT(std::fabs(cap.startBeats - 0.0) < 0.02);
  TEST_ASSERT(std::fabs(cap.endBeats - 2.0) < 0.02);
}

// Engine metronome volume (Phase PR): the click level set on the control
// thread reaches the metronome DSP and reads back the same clamped value.
TEST_CASE(engine_metronome_volume_and_default) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.play(false);
  // Default when omitted: room-level clicks (0.9), enabled with a 2-beat lead.
  engine.setMetronome(true, 2.f);
  std::vector<float> out;
  runBlocks(engine, makeInput(kFpb120), &out);
  TEST_ASSERT(std::fabs(engine.metronomeVolume() - 0.9f) < 1e-6f);
  // Explicit quieter level propagates and clamps out of range.
  engine.setMetronome(true, 2.f, 0.25f);
  runBlocks(engine, makeInput(kFpb120), &out);
  TEST_ASSERT(std::fabs(engine.metronomeVolume() - 0.25f) < 1e-6f);
  engine.setMetronome(true, 2.f, 9.f);
  runBlocks(engine, makeInput(kFpb120), &out);
  TEST_ASSERT(std::fabs(engine.metronomeVolume() - 1.0f) < 1e-6f);
  engine.setMetronome(false, 1.f, 0.5f);
  runBlocks(engine, makeInput(kFpb120), &out);
  TEST_ASSERT(std::fabs(engine.metronomeVolume() - 0.5f) < 1e-6f);
}

// Input monitoring is its own gate: with monitoring off the input is still
// captured into the take but is no longer printed into the output, so the
// (disarmed-monitor) channel is capture-only.
TEST_CASE(record_monitor_off_captures_without_playthrough) {
  constexpr uint32_t kFrames = 4 * kFpb120;

  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.play(false);
  engine.setInputMonitor(false); // capture-only: no playthrough
  engine.startRecord();

  const std::vector<float> in = makeInput(kFrames);
  std::vector<float> out;
  runBlocks(engine, in, &out);
  engine.stopRecord();
  drainStopControls(engine, kFrames);

  // The take is the full input regardless of the monitor gate.
  misuc::engine::RecordCapture cap;
  TEST_ASSERT(engine.takeRecord(cap));
  TEST_ASSERT(cap.samples.size() == kFrames);
  float maxErr = 0.f;
  for (uint32_t i = 0; i < kFrames; ++i)
    maxErr = std::max(maxErr, std::fabs(cap.samples[i] - in[i]));
  TEST_ASSERT(maxErr < 1e-4f);
  // But the output no longer prints the input: past the loop (beats 2..4, no
  // note) the output is silent, and while the loop plays it carries only the
  // loop itself.
  const float pastLoop = peakIn(out, 112000, 144000);
  TEST_ASSERT(pastLoop < 1e-3f);
  TEST_ASSERT(cap.endBeats > 3.5);
}

// Phases G/PR: an offline render must be click-free even when the metronome is
// turned on for playback reference (the clicks are a monitoring aid and are
// never part of the bounce). renderToFile silences the click generator for the
// whole render; a live renderToWav with the metronome enabled must not print
// clicks into the file either.
TEST_CASE(offline_render_excludes_metronome_clicks) {
  auto enginePtr = makeEngine(); AudioEngine& engine = *enginePtr;
  engine.setMetronome(true, 2.0f); // click on, count-in on
  engine.play(false);

  // Render two beats to disk at the native rate. With the metronome enabled
  // through playback, the export must still contain only the loop note.
  const std::string dir = misuc::test::makeTempDir("misuc_offline_render");
  TEST_ASSERT(!dir.empty());
  if (dir.empty())
    return;
  const std::string path = dir + "/clicks.wav";
  std::string err;
  TEST_ASSERT(engine.renderToWav(path, 0.0, 2.0, 2, err));

  // Decode and compare against the loop alone (metronome off): the offline
  // render with the click enabled must equal the click-free loop render.
  static constexpr uint32_t kFrames = 2 * kFpb120;
  auto refPtr = makeEngine(); AudioEngine& ref = *refPtr; // metronome stays off
  ref.play(false);
  std::vector<float> out;
  runBlocks(ref, std::vector<float>(kFrames, 0.f), &out);

  misuc::audio::AudioBuffer wav;
  TEST_ASSERT(misuc::audio::readAudioFile(path, wav, err));
  TEST_ASSERT(wav.frames == kFrames);
  float maxErr = 0.f;
  for (uint32_t i = 0; i < kFrames; ++i)
    maxErr = std::max(maxErr, std::fabs(wav.samples[2 * i] - out[2 * i]));
  TEST_ASSERT(maxErr < 2.0f / 32768.0f);
  std::remove(path.c_str());
}