// Live audio smoke tests for the PipeWire backend. They are compiled only
// where MISUC_BACKEND_PIPEWIRE is defined (the tests talk to the live daemon
// and only report counts; they must never fail a release build when no daemon
// is present). Windows has its own WASAPI equivalents in test_audio_wasapi.
#if defined(MISUC_BACKEND_PIPEWIRE)

#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/AudioEngine.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "test_utils.h"

using misuc::engine::AudioConfig;
using misuc::engine::AudioDeviceInfo;
using misuc::engine::AudioEngine;
using misuc::engine::PipeWireBackend;

// Smoke tests that hit the live PipeWire daemon. They must never fail a release
// build when no daemon is present; they only report counts.
TEST_CASE(device_enumeration) {
  PipeWireBackend backend;
  const auto sinks = backend.enumerateDevices(false);
  const auto sources = backend.enumerateDevices(true);
  std::printf("  found %zu output devices, %zu input devices\n", sinks.size(),
              sources.size());
  bool sane = true;
  for (const AudioDeviceInfo& d : sinks)
    sane = sane && !d.name.empty() && !d.id.empty();
  for (const AudioDeviceInfo& d : sources)
    sane = sane && !d.name.empty() && !d.id.empty();
  TEST_ASSERT(sane);
}

// Regression: opening, running, and tearing down the engine repeatedly must
// not deadlock or corrupt the heap (the loop thread must be quiesced before
// local callback state is freed). Skipped gracefully when no daemon exists.

// CI runners (and any headless box) expose no PipeWire service: open() can
// still succeed against a stub but a real start is impossible without an
// endpoint, so live cases check this first (same policy as the WASAPI suite).
static bool pwEndpointsAvailable() {
  PipeWireBackend probe;
  return !(probe.enumerateDevices(false).empty() &&
           probe.enumerateDevices(true).empty());
}

TEST_CASE(stream_lifecycle) {
  if (!pwEndpointsAvailable()) {
    std::printf("  (no PipeWire endpoints; skipped)\n");
    return;
  }

  AudioEngine engine(std::make_unique<PipeWireBackend>());
  AudioConfig cfg;
  std::string err;
  if (!engine.open(cfg, err)) {
    std::printf("  (no audio daemon; skipped)\n");
    return;
  }
  for (int i = 0; i < 3; ++i) {
    const bool started = engine.start();
    TEST_ASSERT(started);
    if (!started)
      break;
    engine.setTestTone(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    engine.setTestTone(false);
    engine.closeAudio();
    TEST_ASSERT(!engine.isRunning());
  }
}

// Phase 3: groove must produce audible output while the transport plays, and
// the engine must survive enabling/disabling it plus input capture config.
// Daemon-gated like the other live tests.
// Phase 6: the piano-roll score must actually sound while the transport plays,
// through the real-time triple-buffer handoff. Daemon-gated like the others.
TEST_CASE(midi_playback_lifecycle) {
  if (!pwEndpointsAvailable()) {
    std::printf("  (no PipeWire endpoints; skipped)\n");
    return;
  }

  AudioEngine engine(std::make_unique<PipeWireBackend>());
  AudioConfig cfg;
  std::string err;
  if (!engine.open(cfg, err)) {
    std::printf("  (no audio daemon; skipped)\n");
    return;
  }
  engine.setGroove(false); // MIDI alone must be audible
  std::vector<misuc::dsp::MidiNote> notes;
  notes.push_back({0.0, 1.0, misuc::dsp::midiNoteToFreq(55), 0.6f});
  notes.push_back({1.0, 1.0, misuc::dsp::midiNoteToFreq(60), 0.6f});
  notes.push_back({2.0, 1.0, misuc::dsp::midiNoteToFreq(55), 0.6f});
  notes.push_back({3.0, 1.0, misuc::dsp::midiNoteToFreq(62), 0.6f});
  engine.setScore(std::move(notes));

  const bool started = engine.start();
  TEST_ASSERT(started);
  if (!started) {
    engine.closeAudio();
    return;
  }
  engine.play(false);
  // Wait well beyond one device quantum (~170 ms at 48 kHz here) so the meter
  // reliably captures a rendered block; 90 ms sat inside the first quantum and
  // made this daemon-gated check flaky on large-buffer backends.
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const bool audible =
      engine.meterPeakL() > 0.01f || engine.meterRmsL() > 0.f;
  engine.stop();
  engine.clearScore();
  engine.closeAudio();
  TEST_ASSERT(!engine.isRunning());
  TEST_ASSERT(audible);
  std::printf("  midi playback peak %.3f\n", engine.meterPeakL());
}

// Phase 7: note audition must sound through the wall-clock preview path even
// while the transport is stopped, with no score set. Daemon-gated.
TEST_CASE(midi_preview_lifecycle) {
  if (!pwEndpointsAvailable()) {
    std::printf("  (no PipeWire endpoints; skipped)\n");
    return;
  }

  AudioEngine engine(std::make_unique<PipeWireBackend>());
  AudioConfig cfg;
  std::string err;
  if (!engine.open(cfg, err)) {
    std::printf("  (no audio daemon; skipped)\n");
    return;
  }
  engine.setGroove(false);
  const bool started = engine.start();
  TEST_ASSERT(started);
  if (!started) {
    engine.closeAudio();
    return;
  }
  engine.play(false); // audible baseline, ensures the stream is really running
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const float quietPeak =
      std::max(engine.meterPeakL(), engine.meterPeakR());
  engine.stop();
  engine.previewNote(misuc::dsp::midiNoteToFreq(69), 0.9f);
  engine.previewNote(misuc::dsp::midiNoteToFreq(72), 0.9f);
  // The device quantum (~8k frames at 48k) leaves ~170 ms between callbacks,
  // so poll finely across several quanta for the preview to drain and be
  // metered before the meter's fast decay drops the snapshot.
  float previewPeak = 0.f;
  for (int i = 0; i < 400 && previewPeak < 0.05f; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    previewPeak = std::max(std::max(engine.meterPeakL(), engine.meterPeakR()),
                           previewPeak);
  }
  engine.stop();
  engine.closeAudio();
  TEST_ASSERT(!engine.isRunning());
  TEST_ASSERT(previewPeak > 0.01f); // audible while stopped, no score
  TEST_ASSERT(previewPeak > quietPeak);
  std::printf("  preview peak %.3f (quiet %.3f)\n", previewPeak, quietPeak);
}

// Phase 3: groove must produce audible output while the transport plays, and
// the engine must survive enabling/disabling it plus input capture config.
// Daemon-gated like the other live tests.
TEST_CASE(groove_and_capture_lifecycle) {
  if (!pwEndpointsAvailable()) {
    std::printf("  (no PipeWire endpoints; skipped)\n");
    return;
  }

  AudioEngine engine(std::make_unique<PipeWireBackend>());
  AudioConfig cfg;
  cfg.inputDeviceId = "2"; // any real or fake id; only checks it doesn't crash
  std::string err;
  if (!engine.open(cfg, err)) {
    std::printf("  (no audio daemon; skipped)\n");
    return;
  }
  const bool started = engine.start();
  TEST_ASSERT(started);
  if (!started) {
    engine.closeAudio();
    return;
  }
  engine.setGroove(true);
  engine.play(false);
  bool audible = false;
  for (int tries = 0; tries < 200 && !audible; ++tries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    audible = engine.meterPeakL() > 0.01f || engine.meterRmsL() > 0.f;
  }
  engine.setGroove(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  engine.stop();
  TEST_ASSERT(audible);
  engine.closeAudio();
  TEST_ASSERT(!engine.isRunning());
}

#endif // MISUC_BACKEND_PIPEWIRE