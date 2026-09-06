#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace misuc::dsp {

constexpr float kPi = 3.14159265358979323846f;

// RBJ-style biquad filter (audio equalization cookbook). Per-sample,
// allocation-free, real-time safe.
class Biquad {
public:
  enum class Type : uint8_t { Lowpass = 0, Highpass = 1, Bandpass = 2 };

  void setSampleRate(float rate) noexcept { sr_ = rate; }

  // Recalculate coefficients. Callable from RT thread (no allocation).
  void setCoefficients(Type type, float freq, float q) noexcept {
    const float w0 = 2.f * kPi * freq / sr_;
    const float cosw = std::cos(w0);
    const float sinw = std::sin(w0);
    const float alpha = sinw / (2.f * q);

    switch (type) {
      case Type::Lowpass: {
        const float b0 = (1.f - cosw) / 2.f;
        const float b1 = 1.f - cosw;
        const float b2 = (1.f - cosw) / 2.f;
        const float a0 = 1.f + alpha;
        const float a1 = -2.f * cosw;
        const float a2 = 1.f - alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
        break;
      }
      case Type::Highpass: {
        const float b0 = (1.f + cosw) / 2.f;
        const float b1 = -(1.f + cosw);
        const float b2 = (1.f + cosw) / 2.f;
        const float a0 = 1.f + alpha;
        const float a1 = -2.f * cosw;
        const float a2 = 1.f - alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
        break;
      }
      case Type::Bandpass: {
        const float b0 = alpha;
        const float b1 = 0.f;
        const float b2 = -alpha;
        const float a0 = 1.f + alpha;
        const float a1 = -2.f * cosw;
        const float a2 = 1.f - alpha;
        setNormalized(b0, b1, b2, a0, a1, a2);
        break;
      }
    }
  }

  void setFreerun() noexcept { b0_ = 1.f; b1_ = 0.f; b2_ = 0.f; a1_ = 0.f; a2_ = 0.f; }

  void reset() noexcept { x1_ = x2_ = y1_ = y2_ = 0.f; }

  float process(float x) noexcept {
    const float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return y;
  }

  void process(float* data, uint32_t n) noexcept {
    for (uint32_t i = 0; i < n; ++i)
      data[i] = process(data[i]);
  }

private:
  void setNormalized(float b0, float b1, float b2, float a0, float a1, float a2) noexcept {
    const float inv = 1.f / a0;
    b0_ = b0 * inv; b1_ = b1 * inv; b2_ = b2 * inv;
    a1_ = a1 * inv; a2_ = a2 * inv;
  }

  float sr_ = 48000.f;
  float b0_ = 1.f, b1_ = 0.f, b2_ = 0.f, a1_ = 0.f, a2_ = 0.f;
  float x1_ = 0.f, x2_ = 0.f, y1_ = 0.f, y2_ = 0.f;
};

// Peak/RMS level detector with configurable attack/release for metering UI.
class ChannelMeter {
public:
  void setSampleRate(float rate) noexcept { sr_ = rate; }

  void setDecaySeconds(float peakDecay, float rmsWindow) noexcept {
    peakDecayCoef_ = peakDecay <= 0.f ? 0.f : std::exp(-1.f / (peakDecay * sr_));
    rmsCoef_ = rmsWindow <= 0.f ? 0.f : std::exp(-1.f / (rmsWindow * sr_));
  }

  void process(const float* data, uint32_t n) noexcept {
    float peak = 0.f;
    double sumSq = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
      const float a = std::fabs(data[i]);
      peak = std::max(peak, a);
      sumSq += static_cast<double>(data[i]) * data[i];
    }
    accumulate(peak, sumSq, n);
  }

  // Monitor one channel of an interleaved buffer.
  void processInterleaved(const float* data, uint32_t channels, uint32_t index,
                          uint32_t n) noexcept {
    float peak = 0.f;
    double sumSq = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
      const float v = data[i * channels + index];
      const float a = std::fabs(v);
      peak = std::max(peak, a);
      sumSq += static_cast<double>(v) * v;
    }
    accumulate(peak, sumSq, n);
  }

  float peak() const noexcept { return peak_; }
  float rms() const noexcept { return rms_; }
  bool clipped() const noexcept { return clip_; }
  void clearClip() noexcept { clip_ = false; }

private:
  void accumulate(float blockPeak, double sumSq, uint32_t n) noexcept {
    // peak: immediate attack, exponential release
    if (blockPeak >= peak_)
      peak_ = blockPeak;
    else
      peak_ += peakDecayCoef_ * (blockPeak - peak_);

    const double blockRms = std::sqrt(sumSq / static_cast<double>(n));
    rms_ += rmsCoef_ * (static_cast<float>(blockRms) - rms_);
    if (peak_ >= 1.f || blockPeak >= 1.f)
      clip_ = true;
  }

  float sr_ = 48000.f;
  float peakDecayCoef_ = 0.9999f;
  float rmsCoef_ = 0.001f;
  float peak_ = 0.f;
  float rms_ = 0.f;
  bool clip_ = false;
};

inline float dbFromGain(float g) noexcept {
  return g > 0.f ? 20.f * std::log10(g) : -120.f;
}

inline float gainFromDb(float db) noexcept {
  return std::pow(10.f, db / 20.f);
}

inline float fastFabs(float g) noexcept {
  return std::fabs(g);
}

} // namespace misuc::dsp