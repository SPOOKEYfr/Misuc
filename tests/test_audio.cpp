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
TEST_CASE(stream_lifecycle) {
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
TEST_CASE(groove_and_capture_lifecycle) {
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
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const bool audible = engine.meterPeakL() > 0.01f || engine.meterRmsL() > 0.f;
  engine.setGroove(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  engine.stop();
  TEST_ASSERT(audible);
  engine.closeAudio();
  TEST_ASSERT(!engine.isRunning());
}