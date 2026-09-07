#include "test_utils.h"

#include <array>
#include <cmath>

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