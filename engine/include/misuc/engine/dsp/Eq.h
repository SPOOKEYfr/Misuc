#pragma once

#include <cstdint>

#include <misuc/engine/dsp/Dsp.h>

namespace misuc::dsp {

// Real-time-safe built-in parametric EQ insert — the always-available non-LV2
// effect the FX rack offers on every build. Four RBJ peaking bands plus
// optional high/low-pass pads at the chain ends. Configured once on the
// control thread from the published parameter vector (port layout below); the
// audio thread only ever calls process(), which is a pure allocation-free
// biquad chain. A band left at 0 dB (or an edge pad at 20/20000 Hz) is
// transparent, so the default parameter vector is exactly unity.
class ParametricEq {
 public:
  static constexpr uint32_t kBandCount = 4;

  // Port layout (port index == FxParam.index published by the UI):
  static constexpr uint32_t kHpPort = 0;     // high-pass cutoff, Hz
  static constexpr uint32_t kLpPort = 1;     // low-pass cutoff, Hz
  static constexpr uint32_t kBandBase = 2;   // first band's first port
  static constexpr uint32_t kBandFreq = 0;   // +0 band centre, Hz
  static constexpr uint32_t kBandGainDb = 1; // +1 band boost/cut, dB
  static constexpr uint32_t kBandQ = 2;      // +2 band resonance
  static constexpr uint32_t kPortsPerBand = 3;
  static constexpr uint32_t kPortCount =
      kBandBase + kPortsPerBand * kBandCount; // = 14

  // Hard parameter ranges (also advertised to the UI as FxParamInfo min/max).
  static constexpr float kMinHz = 20.f;
  static constexpr float kMaxHz = 20000.f;
  static constexpr float kMinGainDb = -18.f;
  static constexpr float kMaxGainDb = 18.f;
  static constexpr float kMinQ = 0.1f;
  static constexpr float kMaxQ = 12.f;

  // The neutral band centres (equal-tempered-ish spread, Q = 1, gain 0 dB).
  static constexpr float kDefaultBandHz[kBandCount] = {100.f, 250.f,
                                                       1000.f, 4000.f};

  // Write the kPortCount unity defaults into `dst` (used to build the dense
  // parameter vector from a sparse FxParam list).
  static void defaultPorts(float* dst, uint32_t n) noexcept {
    for (uint32_t i = 0; i < n; ++i) dst[i] = 0.f;
    if (n > kHpPort) dst[kHpPort] = kMinHz;
    if (n > kLpPort) dst[kLpPort] = kMaxHz;
    for (uint32_t b = 0; b < kBandCount && kBandBase + kPortsPerBand * b < n;
         ++b) {
      dst[kBandBase + kPortsPerBand * b + kBandFreq] = kDefaultBandHz[b];
      dst[kBandBase + kPortsPerBand * b + kBandGainDb] = 0.f;
      dst[kBandBase + kPortsPerBand * b + kBandQ] = 1.f;
    }
  }

  // `ports` is the dense (length kPortCount) parameter vector; each value is
  // clamped into range before the coefficients are computed. Construction runs
  // on the control thread; the instance is then immutable on the audio thread.
  ParametricEq(float sampleRate, const float* ports, uint32_t n) noexcept {
    setSampleRate(sampleRate);
    setDefaults();
    if (ports && n == kPortCount) {
      hpHz_ = clampHz(ports[kHpPort]);
      lpHz_ = clampHz(ports[kLpPort]);
      for (uint32_t b = 0; b < kBandCount; ++b) {
        bandFreq_[b] =
            clampHz(ports[kBandBase + kPortsPerBand * b + kBandFreq]);
        bandGainDb_[b] = clampDb(
            ports[kBandBase + kPortsPerBand * b + kBandGainDb]);
        bandQ_[b] = clampQ(ports[kBandBase + kPortsPerBand * b + kBandQ]);
      }
    }
    recompute();
    reset();
  }

  void setSampleRate(float rate) noexcept {
    hp_.setSampleRate(rate);
    lp_.setSampleRate(rate);
    for (auto& band : bands_) band.setSampleRate(rate);
  }

  // Render one block; out is in-place. Pure biquad math, no allocations.
  void process(float* l, float* r, uint32_t frames) noexcept {
    for (uint32_t i = 0; i < frames; ++i) {
      hp_.process(l[i], r[i]);
      for (auto& band : bands_) band.process(l[i], r[i]);
      lp_.process(l[i], r[i]);
    }
  }

  // Real-time-safe control-port setter (audio thread, control rate): update one
  // port's value from an automation curve and recompute the impacted stage
  // coefficients. Pure scalar math (no allocation, no locks), so it can be
  // called once per block before process() — exactly how the fx automation
  // path reaches the built-in EQ. Unknown port indices are ignored.
  void setPort(uint32_t port, float value) noexcept {
    if (port == kHpPort) {
      hpHz_ = clampHz(value);
      recompute();
      return;
    }
    if (port == kLpPort) {
      lpHz_ = clampHz(value);
      recompute();
      return;
    }
    if (port >= kBandBase) {
      const uint32_t off = port - kBandBase;
      const uint32_t b = off / kPortsPerBand;
      if (b < kBandCount) {
        const uint32_t k = off % kPortsPerBand;
        if (k == kBandFreq)
          bandFreq_[b] = clampHz(value);
        else if (k == kBandGainDb)
          bandGainDb_[b] = clampDb(value);
        else
          bandQ_[b] = clampQ(value);
        recompute();
      }
    }
  }

 private:
  // One biquad stage for both channels (shared coefficients, separate state).
  struct Stage {
    Biquad la, ra;
    void setSampleRate(float rate) noexcept {
      la.setSampleRate(rate);
      ra.setSampleRate(rate);
    }
    void setPeaking(float f, float db, float q) noexcept {
      la.setPeaking(f, db, q);
      ra.setPeaking(f, db, q);
    }
    void setHz(float f) noexcept {
      la.setCoefficients(Biquad::Type::Lowpass, f, 0.707f);
      ra.setCoefficients(Biquad::Type::Lowpass, f, 0.707f);
    }
    void reset() noexcept {
      la.reset();
      ra.reset();
    }
    void process(float& xl, float& xr) noexcept {
      xl = la.process(xl);
      xr = ra.process(xr);
    }
  };

  static float clampHz(float v) noexcept {
    return v < kMinHz ? kMinHz : v > kMaxHz ? kMaxHz : v;
  }
  static float clampDb(float v) noexcept {
    return v < kMinGainDb ? kMinGainDb : v > kMaxGainDb ? kMaxGainDb : v;
  }
  static float clampQ(float v) noexcept {
    return v < kMinQ ? kMinQ : v > kMaxQ ? kMaxQ : v;
  }

  void setDefaults() noexcept {
    hpHz_ = kMinHz;
    lpHz_ = kMaxHz;
    for (uint32_t b = 0; b < kBandCount; ++b) {
      bandFreq_[b] = kDefaultBandHz[b];
      bandGainDb_[b] = 0.f;
      bandQ_[b] = 1.f;
    }
  }

  void recompute() noexcept {
    // HP edge uses the high-pass shape at the requested cutoff; peaks then
    // boost/cut; LP edge closes the top with a low-pass shape.
    hp_.la.setCoefficients(Biquad::Type::Highpass, hpHz_, 0.707f);
    hp_.ra.setCoefficients(Biquad::Type::Highpass, hpHz_, 0.707f);
    lp_.setHz(lpHz_);
    for (uint32_t b = 0; b < kBandCount; ++b)
      bands_[b].setPeaking(bandFreq_[b], bandGainDb_[b], bandQ_[b]);
  }

  void reset() noexcept {
    hp_.reset();
    lp_.reset();
    for (auto& band : bands_) band.reset();
  }

  float hpHz_ = kMinHz;
  float lpHz_ = kMaxHz;
  float bandFreq_[kBandCount] = {kDefaultBandHz[0], kDefaultBandHz[1],
                                 kDefaultBandHz[2], kDefaultBandHz[3]};
  float bandGainDb_[kBandCount] = {0.f, 0.f, 0.f, 0.f};
  float bandQ_[kBandCount] = {1.f, 1.f, 1.f, 1.f};
  Stage hp_, lp_, bands_[kBandCount];
};

} // namespace misuc::dsp