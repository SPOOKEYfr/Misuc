#include "test_utils.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Groove.h>
#include <misuc/engine/dsp/Metronome.h>
#include <misuc/engine/dsp/Oscillator.h>

using misuc::dsp::Biquad;
using misuc::dsp::ChannelMeter;
using misuc::dsp::Groove;
using misuc::dsp::Oscillator;
using misuc::dsp::Waveform;
using misuc::engine::Metronome;

TEST_CASE(oscillator_sine_bounds) {
  Oscillator osc;
  osc.setSampleRate(48000);
  osc.setFrequency(440);
  osc.setWaveform(Waveform::Sine);
  osc.reset();
  std::array<float, 48000> buf{};
  osc.render(buf.data(), 48000);
  float maxAbs = 0.f;
  for (float v : buf)
    maxAbs = std::max(maxAbs, std::fabs(v));
  TEST_ASSERT(maxAbs <= 1.0001f);
}

TEST_CASE(oscillator_noise_nonzero) {
  Oscillator osc;
  osc.setSampleRate(48000);
  osc.setWaveform(Waveform::Noise);
  osc.render(nullptr, 0); // no crash
  std::array<float, 1000> buf{};
  osc.render(buf.data(), 1000);
  double sum = 0;
  for (float v : buf)
    sum += std::fabs(v);
  TEST_ASSERT(sum > 0.0 && sum < 1000.0);
}

TEST_CASE(biquad_lowpass_removes_highs) {
  Biquad f;
  f.setSampleRate(48000);
  f.setCoefficients(Biquad::Type::Lowpass, 1000.f, 0.707f);
  f.reset();
  // 8 kHz sine should be strongly attenuated after a lowpass at 1 kHz.
  Oscillator osc;
  osc.setSampleRate(48000);
  osc.setFrequency(8000);
  osc.setWaveform(Waveform::Sine);
  osc.reset();
  double before = 0, after = 0;
  for (int i = 0; i < 48000; ++i) {
    const float s = osc.nextSample();
    const float y = f.process(s);
    before += std::fabs(s);
    after += std::fabs(y);
  }
  TEST_ASSERT(after < before * 0.2);
}

TEST_CASE(metronome_clicks_on_beats) {
  Metronome m;
  m.setSampleRate(48000);
  m.setTempo(120); // beat every 24000 frames
  m.setEnabled(true);
  m.setTicking(true);
  m.reset();
  std::array<float, 24000> outL{};
  std::array<float, 24000> outR{};
  m.render(outL.data(), outR.data(), 24000, 1);
  double energyL = 0, energyR = 0;
  for (size_t i = 0; i < outL.size(); ++i) {
    energyL += std::fabs(outL[i]);
    energyR += std::fabs(outR[i]);
  }
  TEST_ASSERT(energyL > 0.0);
  TEST_ASSERT(energyR == energyL); // stereo summed, same signal
}

TEST_CASE(metronome_count_in_completes) {
  Metronome m;
  m.setSampleRate(48000);
  m.setTempo(120);
  m.setEnabled(true);
  m.setTicking(true);
  m.beginCountIn(1); // one beat count-in
  std::array<float, 48000> outL{};
  std::array<float, 48000> outR{};
  m.render(outL.data(), outR.data(), 48000, 1);
  TEST_ASSERT(m.countInComplete());
}

TEST_CASE(metronome_volume_scales_output) {
  Metronome m;
  m.setSampleRate(48000);
  m.setTempo(120);
  m.setEnabled(true);
  m.setTicking(true);
  m.reset();

  auto energy = [&](float volume) {
    m.setVolume(volume);
    m.reset(); // restart the phase so the click lands at frame 0 again
    std::array<float, 24000> outL{};
    std::array<float, 24000> outR{};
    m.render(outL.data(), outR.data(), 24000, 1);
    double e = 0;
    for (float v : outL)
      e += std::fabs(v);
    return e;
  };

  const double quiet = energy(0.1f);
  const double loud = energy(1.0f);
  TEST_ASSERT(quiet > 0.0);
  TEST_ASSERT(loud > quiet * 5.0); // volume is audible in the clicks
  m.setVolume(-1.f);
  TEST_ASSERT(m.volume() == 0.f); // clamps low
  m.setVolume(3.f);
  TEST_ASSERT(m.volume() == 1.f); // clamps high
}

TEST_CASE(groove_disabled_is_silent) {
  Groove g;
  g.setSampleRate(48000);
  g.setTempo(120);
  std::array<float, 48000> outL{};
  std::array<float, 48000> outR{};
  g.render(outL.data(), outR.data(), 48000, 1, 0);
  float maxAbs = 0.f;
  for (float v : outL)
    maxAbs = std::max(maxAbs, std::fabs(v));
  TEST_ASSERT(maxAbs == 0.f);
}

TEST_CASE(groove_hits_have_energy) {
  Groove g;
  g.setSampleRate(48000);
  g.setTempo(120); // beat = 24000 frames, sixteenth = 6000 frames
  g.setEnabled(true);
  std::array<float, 24000> outL{};
  std::array<float, 24000> outR{};
  g.render(outL.data(), outR.data(), 24000, 1, 0);
  // Kick on beat 1 (transient in the first ~6000 frames).
  float energyOn = 0.f;
  for (size_t i = 0; i < 8192; ++i)
    energyOn += std::fabs(outL[i]);
  TEST_ASSERT(energyOn > 50.f);
}

TEST_CASE(groove_is_deterministic) {
  Groove a;
  a.setSampleRate(48000);
  a.setTempo(120);
  a.setEnabled(true);
  Groove b;
  b.setSampleRate(48000);
  b.setTempo(120);
  b.setEnabled(true);
  std::array<float, 24000> outA{};
  std::array<float, 48000> outB{};
  a.render(outA.data(), outA.data(), 24000, 1, 0);
  b.render(outB.data(), outB.data(), 24000, 2, 0);
  for (size_t i = 0; i < 24000; ++i) {
    // Stereo (stride 2) duplicates the mono (stride 1) signal on both channels.
    TEST_ASSERT(std::fabs(outB[i * 2] - outA[i]) < 1e-4f);
    TEST_ASSERT(std::fabs(outB[i * 2 + 1] - outA[i]) < 1e-4f);
  }
}

TEST_CASE(meter_peak_and_rms) {
  ChannelMeter meter;
  meter.setSampleRate(48000);
  meter.setDecaySeconds(0.1f, 600.0f); // near-instant RMS tracking for the test
  std::array<float, 48000> buf{};
  for (size_t i = 0; i < buf.size(); ++i)
    buf[i] = 0.5f;
  meter.process(buf.data(), 48000);
  TEST_ASSERT(std::fabs(meter.peak() - 0.5f) < 0.01f);
  TEST_ASSERT(std::fabs(meter.rms() - 0.5f) < 0.01f);
}

TEST_CASE(master_limiter_brickwall_ceiling) {
  misuc::dsp::MasterLimiter lim;
  constexpr uint32_t n = 48000;
  std::array<float, n> buf{};
  for (uint32_t i = 0; i < n; ++i)
    buf[i] = 1.5f; // would hard-clip without the limiter
  lim.process(buf.data(), n, 1);
  float peak = 0.f;
  for (float v : buf)
    peak = std::max(peak, std::fabs(v));
  TEST_ASSERT(peak <= lim.ceiling() + 1e-5f); // nothing above the ceiling
  TEST_ASSERT(peak > 0.80f);                  // and it still delivers signal
  TEST_ASSERT(lim.currentGain() < 1.0f);      // limiter actually engaged
}

TEST_CASE(master_limiter_passes_below_ceiling) {
  misuc::dsp::MasterLimiter lim;
  constexpr uint32_t n = 48000;
  std::array<float, n> buf{};
  for (uint32_t i = 0; i < n; ++i)
    buf[i] = 0.25f;
  lim.process(buf.data(), n, 1);
  TEST_ASSERT(std::fabs(buf[n - 1] - 0.25f) < 1e-3f); // untouched
  TEST_ASSERT(lim.currentGain() > 0.99f);
}

TEST_CASE(master_limiter_release_recovers) {
  misuc::dsp::MasterLimiter lim;
  constexpr uint32_t burst = 240;  // ~5 ms of a full-scale overload
  constexpr uint32_t rest = 96000; // 2 s of silence afterwards
  std::array<float, burst + rest> buf{};
  for (uint32_t i = 0; i < burst; ++i)
    buf[i] = 1.0f;
  lim.process(buf.data(), burst + rest, 1);
  TEST_ASSERT(lim.currentGain() > 0.99f); // released back to unity
}

TEST_CASE(interpolated_peak_never_below_block_peak) {
  misuc::dsp::ChannelMeter meter;
  meter.setSampleRate(48000);
  meter.setDecaySeconds(0.1f, 600.0f);
  std::array<float, 9600> buf{};
  for (uint32_t i = 0; i < buf.size(); ++i)
    buf[i] = (i % 7 == 0) ? 0.95f : static_cast<float>(i % 13) / 13.f;
  meter.processInterleavedTruePeak(buf.data(), 1, 0, buf.size());
  float blockPeak = 0.f;
  for (float v : buf)
    blockPeak = std::max(blockPeak, std::fabs(v));
  TEST_ASSERT(meter.peak() >= blockPeak - 1e-4f);
}

TEST_CASE(resample_linear_preserves_sine_pitch) {
  // 2 s of a 440 Hz sine at 48k, resampled down to 44.1k: the frequency (and
  // the sample count of the file, and therefore the duration) is preserved.
  constexpr uint32_t inSr = 48000, outSr = 44100;
  constexpr double freq = 440.0;
  constexpr uint64_t inFrames = 2 * static_cast<uint64_t>(inSr);
  const uint64_t outFrames =
      static_cast<uint64_t>(std::llround(static_cast<double>(inFrames) *
                                         outSr / inSr));
  std::vector<float> in(inFrames);
  for (uint64_t i = 0; i < inFrames; ++i)
    in[i] = std::sin(2.0 * 3.14159265358979 * freq *
                     static_cast<double>(i) / inSr);
  std::vector<float> out(outFrames);
  misuc::dsp::resampleLinear(in.data(), inFrames, 1, outFrames, out.data());

  // 2 s at 44100 → 88200 frames.
  TEST_ASSERT(outFrames == 88200);

  // Count sign changes in the resampled mono: ~2 * 440 per second over 2 s.
  uint64_t crossings = 0;
  for (uint64_t i = 1; i < outFrames; ++i) {
    if ((out[i - 1] < 0.0f) != (out[i] < 0.0f))
      ++crossings;
  }
  const uint64_t expect = 4 * static_cast<uint64_t>(freq); // 2 crossings / Hz
  TEST_ASSERT(std::fabs(static_cast<long long>(crossings) -
                        static_cast<long long>(expect)) <= 8);

  // Raw peak survives within a couple of LSBs of the interpolated signal.
  float peak = 0.f;
  for (float v : out)
    peak = std::max(peak, std::fabs(v));
  TEST_ASSERT(peak > 0.99f);

  // Upsampling (44.1k → 96k) keeps the frequency too (crossing-per-second
  // still ~2*freq) and adds no NaN.
  constexpr uint32_t outSrUp = 96000;
  const uint64_t upFrames = static_cast<uint64_t>(std::llround(
      static_cast<double>(outFrames) * outSrUp / outSr));
  std::vector<float> up(upFrames);
  misuc::dsp::resampleLinear(out.data(), outFrames, 1, upFrames, up.data());
  uint64_t upCrossings = 0;
  for (uint64_t i = 1; i < upFrames; ++i) {
    if ((up[i - 1] < 0.0f) != (up[i] < 0.0f))
      ++upCrossings;
  }
  TEST_ASSERT(std::fabs(static_cast<long long>(upCrossings) -
                        static_cast<long long>(expect)) <= 8);
  float upPeak = 0.f;
  for (float v : up)
    upPeak = std::max(upPeak, std::fabs(v));
  TEST_ASSERT(!std::isnan(upPeak));
  TEST_ASSERT(upPeak > 0.99f);
}