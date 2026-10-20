// WAV file read/write paths (closes the documented coverage gap: readAudioFile
// had no direct tests). Exercises libsndfile through the engine's WavFile.h
// facade: float round-trip, PCM16 quantization round-trip, missing-file error
// reporting and invalid-parameter rejection.

#include <misuc/engine/audio/WavFile.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "test_utils.h"

using misuc::audio::AudioBuffer;
using misuc::audio::readAudioFile;
using misuc::audio::writeWavSamples;

namespace {

// Runtime path: temp dir must exist before the file lands (POSIX-style
// constant "/tmp/..." is not creatable on Windows).
static const std::string kDir = misuc::test::makeTempDir("misuc_wavfile_test");
static const std::string kPathStr = kDir + "/test.wav";
#define kPath kPathStr.c_str()
constexpr double kPi = 3.14159265358979323846;

std::vector<float> tone(uint64_t frames, float loopAmp, float rightScale) {
  std::vector<float> s(frames * 2);
  for (uint64_t i = 0; i < frames; ++i) {
    const float v = std::sin(float(2.0 * kPi * 440.0 * double(i) / 48000.0)) *
                    loopAmp;
    s[i * 2] = v;
    s[i * 2 + 1] = v * rightScale;
  }
  return s;
}

} // namespace

TEST_CASE(wavfile_float_roundtrip) {
  std::remove(kPath);
  const uint64_t frames = 4800;
  const auto src = tone(frames, 0.5f, 0.25f);
  std::string err;
  TEST_ASSERT(writeWavSamples(kPath, src.data(), frames, 2, 48000.0, false,
                              err));
  AudioBuffer out;
  TEST_ASSERT(readAudioFile(kPath, out, err));
  TEST_ASSERT(out.frames == frames);
  TEST_ASSERT(out.channels == 2);
  TEST_ASSERT(out.sampleRate == 48000.0);
  bool same = true;
  for (uint64_t i = 0; i < frames && same; ++i)
    same = std::fabs(out.samples[i] - src[i]) < 1e-4;
  TEST_ASSERT(same);
  std::remove(kPath);
}

TEST_CASE(wavfile_pcm16_roundtrip_quantized) {
  std::remove(kPath);
  const uint64_t frames = 960;
  const auto src = tone(frames, 0.8f, 1.0f);
  std::string err;
  TEST_ASSERT(writeWavSamples(kPath, src.data(), frames, 2, 48000.0, true,
                              err));
  AudioBuffer out;
  TEST_ASSERT(readAudioFile(kPath, out, err));
  TEST_ASSERT(out.frames == frames);
  TEST_ASSERT(out.channels == 2);
  TEST_ASSERT(out.sampleRate == 48000.0);
  // PCM16 quantisation error is bounded by half an LSB (1/32768 of full
  // scale); compare inside a comfortably wider window.
  bool quantOk = true;
  for (uint64_t i = 0; i < frames && quantOk; ++i)
    quantOk = std::fabs(out.samples[i] - src[i]) < 6e-5;
  TEST_ASSERT(quantOk);
  std::remove(kPath);
}

TEST_CASE(wavfile_missing_file_reports_error) {
  std::remove(kPath);
  AudioBuffer out;
  std::string err;
  TEST_ASSERT(!readAudioFile(kPath, out, err));
  TEST_ASSERT(!err.empty());
  TEST_ASSERT(out.frames == 0);
}

TEST_CASE(wavfile_invalid_export_params_rejected) {
  std::string err;
  TEST_ASSERT(!writeWavSamples(kPath, nullptr, 0, 0, 0.0, false, err));
  TEST_ASSERT(!err.empty());
  err.clear();
  TEST_ASSERT(!writeWavSamples(kPath, nullptr, 48000, 0, 48000.0, false,
                               err));
  TEST_ASSERT(!err.empty());
  err.clear();
  TEST_ASSERT(!writeWavSamples(kPath, nullptr, 48000, 2, 48000.0, false,
                               err));
  TEST_ASSERT(!err.empty());
}