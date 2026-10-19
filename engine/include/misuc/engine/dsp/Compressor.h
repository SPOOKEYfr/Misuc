#pragma once

#include <cmath>
#include <cstdint>

#include <misuc/engine/dsp/Dsp.h>

namespace misuc::dsp {

// Real-time-safe built-in sidechain-capable compressor insert
// ("builtin:sc-compressor"). Peak detector with attack/release smoothing
// drives a feed-forward gain reduction curve; with a sidechain key signal
// (another bus's post-fader output, e.g. the kick bus driving a bass bus) the
// detector watches the KEY instead of the input — classic ducking. Without a
// key the detector watches the input, so the same DSP is a plain compressor.
//
// Port layout (port index == FxParam.index published by the UI):
//   0 threshold dB  (-60..0,   def -24)
//   1 ratio         (1..20,    def 4)
//   2 attack ms     (0..100,   def 5)
//   3 release ms    (10..1000, def 200)
//   4 makeup dB     (0..12,    def 0)
// Constructed once on the control thread from the dense parameter vector; the
// audio thread only calls process()/setPort() (pure allocation-free math).
class Compressor {
 public:
  static constexpr uint32_t kPortCount = 5;
  static constexpr uint32_t kThresholdDbPort = 0;
  static constexpr uint32_t kRatioPort = 1;
  static constexpr uint32_t kAttackMsPort = 2;
  static constexpr uint32_t kReleaseMsPort = 3;
  static constexpr uint32_t kMakeupDbPort = 4;

  static constexpr float kMinThresholdDb = -60.f;
  static constexpr float kMaxThresholdDb = 0.f;
  static constexpr float kMinRatio = 1.f;
  static constexpr float kMaxRatio = 20.f;
  static constexpr float kMinAttackMs = 0.f;
  static constexpr float kMaxAttackMs = 100.f;
  static constexpr float kMinReleaseMs = 10.f;
  static constexpr float kMaxReleaseMs = 1000.f;
  static constexpr float kMinMakeupDb = 0.f;
  static constexpr float kMaxMakeupDb = 12.f;

  static void defaultPorts(float* dst, uint32_t n) noexcept {
    for (uint32_t i = 0; i < n; ++i) dst[i] = 0.f;
    if (n > kThresholdDbPort) dst[kThresholdDbPort] = -24.f;
    if (n > kRatioPort) dst[kRatioPort] = 4.f;
    if (n > kAttackMsPort) dst[kAttackMsPort] = 5.f;
    if (n > kReleaseMsPort) dst[kReleaseMsPort] = 200.f;
    if (n > kMakeupDbPort) dst[kMakeupDbPort] = 0.f;
  }

  Compressor(float sampleRate, const float* ports, uint32_t n) noexcept {
    setSampleRate(sampleRate);
    reset();
    if (ports && n == kPortCount) {
      setThresholdDb(ports[kThresholdDbPort]);
      setRatio(ports[kRatioPort]);
      setAttackMs(ports[kAttackMsPort]);
      setReleaseMs(ports[kReleaseMsPort]);
      setMakeupDb(ports[kMakeupDbPort]);
    }
  }

  void setSampleRate(float rate) noexcept {
    sr_ = rate > 0.f ? rate : 48000.f;
    recomputeTimes();
  }

  void reset() noexcept {
    env_ = 0.f;
    gainDb_ = 0.f;
  }

  // Live control-port setter (audio thread, control rate): one port's value
  // from an automation curve, exactly like ParametricEq::setPort. Unknown
  // indices are ignored.
  void setPort(uint32_t port, float value) noexcept {
    switch (port) {
      case kThresholdDbPort: setThresholdDb(value); break;
      case kRatioPort: setRatio(value); break;
      case kAttackMsPort: setAttackMs(value); break;
      case kReleaseMsPort: setReleaseMs(value); break;
      case kMakeupDbPort: setMakeupDb(value); break;
      default: break;
    }
  }

  // Render one block; out is in-place and the key is the input itself (plain
  // compression). Pure scalar math, no allocations.
  void process(float* l, float* r, uint32_t frames) noexcept {
    process(l, r, l, r, frames);
  }

  // Render one block keyed by an external signal (`keyL`/`keyR`, a sidechain
  // bus's post-fader output). With no active sidechain the caller passes the
  // input itself. Recomputes curve coefficients at the start of the block so
  // automation control values take effect immediately.
  void process(float* l, float* r, const float* keyL, const float* keyR,
               uint32_t frames) noexcept {
    recomputeTimes();
    for (uint32_t i = 0; i < frames; ++i) {
      const float det = std::max(std::fabs(keyL[i]), std::fabs(keyR[i]));
      const float attack = det > env_;
      const float env = env_ + (det - env_) * (attack ? aCoef_ : rCoef_);
      env_ = env;
      const float detDb = env > 1e-6f ? 20.f * std::log10(env) : -120.f;
      const float over = detDb - thrDb_; // dB above threshold
      float gr = 0.f;
      if (over > 0.f)
        gr = -(1.f - 1.f / ratio_) * over; // feed-forward static curve
      const float g = kMakeupLin_ * gainFromDb(gr);
      l[i] *= g;
      r[i] *= g;
    }
  }

 private:
  void clamp_() noexcept {
    thrDb_ = std::clamp(thrDb_, kMinThresholdDb, kMaxThresholdDb);
    ratio_ = std::clamp(ratio_, kMinRatio, kMaxRatio);
    attackMs_ = std::clamp(attackMs_, kMinAttackMs, kMaxAttackMs);
    releaseMs_ = std::clamp(releaseMs_, kMinReleaseMs, kMaxReleaseMs);
    kMakeupLin_ = gainFromDb(std::clamp(makeupDb_, kMinMakeupDb, kMaxMakeupDb));
  }
  void recomputeTimes() noexcept {
    const float at = attackMs_ * 0.001f;
    const float rt = releaseMs_ * 0.001f;
    aCoef_ = at > 0.f ? 1.f - std::exp(-1.f / (at * sr_)) : 1.f;
    rCoef_ = rt > 0.f ? 1.f - std::exp(-1.f / (rt * sr_)) : 1.f;
  }
  void setThresholdDb(float v) noexcept { thrDb_ = v; clamp_(); }
  void setRatio(float v) noexcept { ratio_ = v; clamp_(); }
  void setAttackMs(float v) noexcept { attackMs_ = v; clamp_(); recomputeTimes(); }
  void setReleaseMs(float v) noexcept { releaseMs_ = v; clamp_(); recomputeTimes(); }
  void setMakeupDb(float v) noexcept { makeupDb_ = v; clamp_(); }

  float sr_ = 48000.f;
  float thrDb_ = -24.f;
  float ratio_ = 4.f;
  float attackMs_ = 5.f;
  float releaseMs_ = 200.f;
  float makeupDb_ = 0.f;
  float kMakeupLin_ = 1.f;
  float aCoef_ = 0.f;
  float rCoef_ = 0.f;
  float env_ = 0.f;
  float gainDb_ = 0.f;
};

} // namespace misuc::dsp