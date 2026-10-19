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

  // RBJ peaking (parametric) band: boosts/cuts `gainDb` at `freq` with
  // resonance `q`. At 0 dB the band degenerates to an exact pass-through.
  void setPeaking(float freq, float gainDb, float q) noexcept {
    const float a = std::pow(10.f, gainDb / 40.f);
    const float w0 = 2.f * kPi * freq / sr_;
    const float cosw = std::cos(w0);
    const float alpha = std::sin(w0) / (2.f * q);
    const float b0 = 1.f + alpha * a;
    const float b1 = -2.f * cosw;
    const float b2 = 1.f - alpha * a;
    const float a0 = 1.f + alpha / a;
    const float a1 = -2.f * cosw;
    const float a2 = 1.f - alpha / a;
    setNormalized(b0, b1, b2, a0, a1, a2);
  }

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

// Inter-sample true-peak estimate of one channel slice of an interleaved
// buffer, using a Catmull-Rom cubic between adjacent samples evaluated at
// u = 1/2 (and falling back to the block peak). Cheap, allocation-free, and a
// closer bound on the analogue signal than the plain block peak: for spiky
// material it reads a fraction of a dB above the block peak.
inline float interpolatedPeakChannel(const float* data, uint32_t channels,
                                     uint32_t index, uint32_t n) noexcept {
  if (n == 0)
    return 0.f;
  float tp = 0.f;
  for (uint32_t i = 0; i < n; ++i) {
    const float samplePeak = std::fabs(data[i * channels + index]);
    if (samplePeak > tp)
      tp = samplePeak;
  }
  if (n < 2)
    return tp;
  for (uint32_t i = 1; i < n; ++i) {
    const float p1 = data[(i - 1) * channels + index];
    const float p2 = data[i * channels + index];
    if (p1 * p2 < 0.f) // zero crossing: no inter-sample apex worth scoring
      continue;
    const float p0 = (i >= 2) ? data[(i - 2) * channels + index] : p1;
    const float p3 = (i + 1 < n) ? data[(i + 1) * channels + index] : p2;
    // Catmull-Rom: p(u) = 0.5*(2p1 + (-p0+p2)u + (2p0-5p1+4p2-p3)u^2
    //                         + (-p0+3p1-3p2+p3)u^3)
    const float A = 0.5f * (-p0 + p2);
    const float B = 0.5f * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3);
    const float C = 0.5f * (-p0 + 3.f * p1 - 3.f * p2 + p3);
    const float u = 0.5f;
    const float up = p1 + u * A + (u * u) * B + (u * u * u) * C;
    const float mid = std::fabs(up);
    if (mid > tp)
      tp = mid;
  }
  return tp;
}

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

  // Monitor one channel of an interleaved buffer, folding an inter-sample
  // peak estimate into the reported peak so near-0 dBFS meters flag clip risk
  // earlier. The block peak always defines the floor.
  void processInterleavedTruePeak(const float* data, uint32_t channels,
                                  uint32_t index, uint32_t n) noexcept {
    float peak = 0.f;
    double sumSq = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
      const float v = data[i * channels + index];
      const float a = std::fabs(v);
      peak = std::max(peak, a);
      sumSq += static_cast<double>(v) * v;
    }
    const float tp = interpolatedPeakChannel(data, channels, index, n);
    accumulate(std::max(peak, tp), sumSq, n);
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

// Constant-power pan law. `pan` in [-1,+1] maps to an angle in [0, PI/2]; the
// two channel gains square-sum to 1 so centre (-3 dB per channel) is balanced
// and no direction is louder. Returns (gL, gR).
inline std::pair<float, float> panLaw(float pan) noexcept {
  const float p = std::clamp(pan, -1.f, 1.f);
  const float a = (p + 1.f) * kPi / 4.f;
  return {std::cos(a), std::sin(a)};
}

// Rational piecewise-linear resampler over interleaved `channels` frames.
// Maps output sample o in [0, outFrames) to the input time base
// o*(inFrames-1)/(outFrames-1), preserving the endpoint samples and, because
// the caller writes the industry-standard output frame rate alongside, the
// absolute duration and pitch of the signal. Offline-only (allocation-free
// but not a bandlimited antialiasing filter; fine for render-rate conversion).
// out must hold at least outFrames*channels floats.
inline void resampleLinear(const float* in, size_t inFrames,
                           uint32_t channels, size_t outFrames,
                           float* out) noexcept {
  if (inFrames == 0 || outFrames == 0 || channels == 0) {
    return;
  }
  if (inFrames == 1) {
    for (size_t o = 0; o < outFrames; ++o)
      for (uint32_t c = 0; c < channels; ++c)
        out[o * channels + c] = in[c];
    return;
  }
  const double step =
      static_cast<double>(inFrames - 1) / static_cast<double>(outFrames - 1);
  for (size_t o = 0; o < outFrames; ++o) {
    const double pos = static_cast<double>(o) * step;
    const size_t fi = std::min<size_t>(static_cast<size_t>(pos), inFrames - 2);
    const float frac = static_cast<float>(pos - static_cast<double>(fi));
    for (uint32_t c = 0; c < channels; ++c) {
      const float a = in[fi * channels + c];
      const float b = in[(fi + 1) * channels + c];
      out[o * channels + c] = a + frac * (b - a);
    }
  }
}

inline float fastFabs(float g) noexcept {
  return std::fabs(g);
}

// Feed-forward brickwall peak limiter for a master/insert bus stage. Instant
// attack engages on the very sample that would exceed the ceiling (so no
// output sample ever exceeds it); the gain recovers with an exponential
// release so sustained loud material does not zipper. Processing is
// allocation-free and in-place, identical for live and offline rendering.
struct MasterLimiter {
  void reset() noexcept { gain_ = 1.f; }
  void setCeiling(float ceiling) noexcept {
    ceiling_ = std::clamp(ceiling, 1e-3f, 1.f);
  }
  void setReleaseSeconds(float seconds, float sr) noexcept {
    releaseCoef_ =
        std::exp(-1.f / std::max(seconds, 5e-4f) / std::max(sr, 1.f));
  }
  float ceiling() const noexcept { return ceiling_; }
  float currentGain() const noexcept { return gain_; }

  // Processes `frames` interleaved frames (1 or 2 channels). Must not be
  // called from two threads on the same instance.
  void process(float* data, uint32_t frames, uint32_t channels) noexcept {
    if (channels == 0 || frames == 0)
      return;
    for (uint32_t f = 0; f < frames; ++f) {
      float peak = 0.f;
      for (uint32_t c = 0; c < channels; ++c) {
        const float a = std::fabs(data[f * channels + c]);
        peak = std::max(peak, a);
      }
      const float target =
          (peak > ceiling_ && peak > 0.f) ? ceiling_ / peak : 1.f;
      if (target < gain_)
        gain_ = target;
      else
        gain_ += releaseCoef_ * (target - gain_);
      for (uint32_t c = 0; c < channels; ++c)
        data[f * channels + c] *= gain_;
    }
  }

private:
  float ceiling_ = 0.891f; // -1 dBFS
  float releaseCoef_ = std::exp(-1.f / (0.25f * 48000.f));
  float gain_ = 1.f;
};

} // namespace misuc::dsp