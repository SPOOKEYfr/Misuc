#pragma once

#include <cmath>
#include <cstdint>

namespace misuc::dsp {

// Phase-accumulator oscillator. Loop-free, allocation-free, real-time safe.
enum class Waveform : uint8_t {
  Sine = 0,
  Triangle = 1,
  Saw = 2,
  Square = 3,
  Noise = 4,
};

inline float bipolarToWarped(float t, float warpPct) noexcept {
  // subtle smoothstep-like warp, unbiased from -100%..+100% -> -1..1
  return std::cos(warpPct * 0.75f * (1.f + t)) - 0.5f;
}

class Oscillator {
public:
  void setSampleRate(double rate) noexcept { sampleRate_ = rate; }
  void setFrequency(double freq) noexcept {
    phaseInc_ = freq / sampleRate_;
  }
  void setWaveform(Waveform w) noexcept { wave_ = w; }
  void reset() noexcept { phase_ = 0.0; }

  // Mono render into out (nFrames).
  void render(float* out, uint32_t nFrames) noexcept {
    for (uint32_t i = 0; i < nFrames; ++i)
      out[i] = nextSample();
  }

  float nextSample() noexcept {
    phase_ += phaseInc_;
    if (phase_ >= 1.0)
      phase_ -= std::floor(phase_);
    const double p = phase_;
    switch (wave_) {
      case Waveform::Sine: {
        const double s = std::sin(2.0 * 3.14159265358979323846 * p);
        return static_cast<float>(s);
      }
      case Waveform::Triangle:
        return static_cast<float>(4.0 * std::fabs(p - 0.5) - 1.0);
      case Waveform::Saw:
        return static_cast<float>(2.0 * p - 1.0);
      case Waveform::Square:
        return p < 0.5 ? 1.0f : -1.0f;
      case Waveform::Noise:
        // xorshift32 white noise
        state_ = state_ ? state_ : 0x9e3779b9u;
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(static_cast<int32_t>(state_)) * (1.f / 2147483648.f);
    }
    return 0.f;
  }

  void setNoiseSeed(uint32_t seed) noexcept { state_ = seed; }

private:
  double sampleRate_ = 48000.0;
  double phaseInc_ = 0.0;
  double phase_ = 0.0;
  Waveform wave_ = Waveform::Sine;
  uint32_t state_ = 0x9e3779b9u;
};

} // namespace misuc::dsp