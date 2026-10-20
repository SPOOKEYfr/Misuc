// Live audio smoke tests for the WASAPI backend, Windows only. Mirror the
// PipeWire tests: they must never fail a release build when no audio device
// is present (headless CI runners are common); they only report counts.
#if defined(_WIN32) && defined(MISUC_BACKEND_WASAPI)

#include <misuc/engine/audio/WASAPIBackend.h>
#include <misuc/engine/audio/AudioEngine.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "test_utils.h"

using misuc::engine::AudioConfig;
using misuc::engine::AudioDeviceInfo;
using misuc::engine::AudioEngine;
using misuc::engine::WASAPIBackend;

TEST_CASE(wasapi_device_enumeration) {
  WASAPIBackend backend;
  const auto sinks = backend.enumerateDevices(false);
  const auto sources = backend.enumerateDevices(true);
  std::printf("  found %zu output devices, %zu input devices\n", sinks.size(),
              sources.size());
  if (sinks.empty()) {
    std::printf("  (no WASAPI endpoint; skipped)\n");
    return;
  }
  bool sane = true;
  for (const AudioDeviceInfo& d : sinks)
    sane = sane && !d.name.empty() && !d.id.empty();
  for (const AudioDeviceInfo& d : sources)
    sane = sane && !d.name.empty() && !d.id.empty();
  TEST_ASSERT(sane);
}

TEST_CASE(wasapi_stream_lifecycle) {
  WASAPIBackend backend;
  AudioConfig cfg;
  std::string err;
  // Headless runners can sometimes expose a phantom default endpoint that
  // "opens" but has zero enumerable devices; a real render cycle is not
  // deducible without a device, so skip instead of validating nothing.
  if (backend.enumerateDevices(false).empty() &&
      backend.enumerateDevices(true).empty()) {
    std::printf("  (no WASAPI endpoint; skipped)\n");
    return;
  }
  if (!backend.open(cfg, err)) {
    std::printf("  (no WASAPI endpoint; skipped: %s)\n", err.c_str());
    return;
  }
  // Start requires a live renderer; reuse the engine so the callback real
  // path is exercised, exactly like the PipeWire stream lifecycle test.
  AudioEngine engine(std::make_unique<WASAPIBackend>());
  err.clear();
  cfg.sampleRate = 48000;
  cfg.bufferSize = 512;
  if (!engine.open(cfg, err)) {
    std::printf("  (no WASAPI endpoint; skipped: %s)\n", err.c_str());
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
  std::printf("  wasapi rate %u, frames %u, xruns %u\n",
              engine.backendStatus().negotiatedRate,
              engine.backendStatus().negotiatedBuffer,
              engine.backendStatus().xruns);
}

#endif //_WIN32 && MISUC_BACKEND_WASAPI
