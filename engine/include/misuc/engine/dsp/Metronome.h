#pragma once

#include <cmath>
#include <cstdint>

#include <misuc/engine/dsp/Dsp.h>

namespace misuc::engine {

// Sample-accurate click metronome. Handles beat phase internally so clicks land
// exactly on musical boundaries. Allocation-free and real-time safe.
// Generates a percussive click with fast attack/short decay for professional feel.
class Metronome {
public:
void setSampleRate(double sr) noexcept {
    sampleRate_ = sr > 0.0 ? sr : 48000.0;
    spb_ = framesPerBeat();
    updateDecay();
  }

  void updateDecay() noexcept {
    // 30ms decay time for professional percussive feel
    decay_ = std::exp(-1.f / (0.03f * static_cast<float>(sampleRate_)));
  }

  void setTempo(double bpm) noexcept {
    bpm_ = bpm < 20.0 ? 20.0 : (bpm > 400.0 ? 400.0 : bpm);
    spb_ = framesPerBeat();
    updateDecay();
  }

  void setTimeSignature(int num, int den) noexcept {
    tsNum_ = num > 0 ? num : 4;
    tsDen_ = den > 0 ? den : 4;
  }

  void setVolume(float v) noexcept { volume_ = (v < 0.f ? 0.f : (v > 1.f ? 1.f : v)); }
  float volume() const noexcept { return volume_; }
  void setEnabled(bool on) noexcept { enabled_ = on; }
  bool enabled() const noexcept { return enabled_; }
  void setTicking(bool on) noexcept { ticking_ = on; }

  void reset() noexcept {
    phase_ = 0.0;
    beat_ = 0;
    clickEnv_ = 0.0;
    pendingClick_ = true;
  }

  // Start a count-in of `beats` beats from the downbeat. Clicks from t = 0.
  void beginCountIn(uint32_t beats) noexcept {
    countInTotal_ = beats;
    countInRemaining_ = beats;
    phase_ = 0.0;
    beat_ = 0;
    pendingClick_ = true;
  }
  bool inCountIn() const noexcept { return countInTotal_ > 0; }
  bool countInComplete() const noexcept {
    return countInTotal_ > 0 && countInRemaining_ == 0;
  }
  void clearCountIn() noexcept { countInTotal_ = 0; countInRemaining_ = 0; }

  void setPendingClick(bool on) noexcept { pendingClick_ = on; }

  // Render clicks into two interleaved channels with `stride` samples between
  // consecutive channel samples (stride == channels). Additive output.
  void render(float* outL, float* outR, uint32_t frames, uint32_t stride) noexcept {
    if (!enabled_ || !ticking_ || spb_ < 1.0)
      return;

    const float vol = volume_;
    const float fNormal = clickFreqNormal_ / static_cast<float>(sampleRate_);
    const float fAccent = clickFreqAccent_ / static_cast<float>(sampleRate_);

    for (uint32_t i = 0; i < frames; ++i) {
      if (pendingClick_) {
        trigger(beat_ == 0, fNormal, fAccent);
        pendingClick_ = false;
      }
      phase_ += 1.0;
      while (phase_ >= spb_) {
        phase_ -= spb_;
        beat_ = (beat_ + 1) % tsNum_;
        if (inCountIn() && countInRemaining_ > 0)
          --countInRemaining_;
        trigger(beat_ == 0, fNormal, fAccent);
      }

      if (clickEnv_ <= 1e-5f)
        continue;

      // Percussive envelope: fast attack (handled by trigger), exponential decay
      clickEnv_ *= decay_;

      // Bandlimited oscillator for cleaner tone
      clickPhase_ += clickFreq_;
      if (clickPhase_ >= 1.0)
        clickPhase_ -= std::floor(clickPhase_);

      // Soft-sync style tone: sine with slight harmonic content
      const float base = std::sin(2.f * dsp::kPi * static_cast<float>(clickPhase_));
      // Add 2nd harmonic for richer "click" character
      const float harm = 0.3f * std::sin(4.f * dsp::kPi * static_cast<float>(clickPhase_));
      const float s = (base + harm) * clickEnv_ * vol;

      // High-pass to remove DC and emphasize transient
      const float y = s - 0.995f * hpPrev_;
      hpPrev_ = s;

      outL[i * stride] += y;
      outR[i * stride] += y;

      if (clickEnv_ < 1e-5f)
        clickEnv_ = 0.f;
    }
  }

  // Trigger a click at the current position
  void triggerClick(bool accent) noexcept {
    trigger(accent, clickFreqNormal_ / static_cast<float>(sampleRate_),
            clickFreqAccent_ / static_cast<float>(sampleRate_));
  }

  // Getters for UI
  float clickVolume() const noexcept { return volume_; }
  bool isTicking() const noexcept { return ticking_; }
  bool isEnabled() const noexcept { return enabled_; }
  uint32_t countInTotal() const noexcept { return countInTotal_; }
  uint32_t countInRemaining() const noexcept { return countInRemaining_; }
  int currentBeat() const noexcept { return beat_; }
  int timeSigNumerator() const noexcept { return tsNum_; }
  int timeSigDenominator() const noexcept { return tsDen_; }

private:
  void trigger(bool accent, float fNormal, float fAccent) noexcept {
    clickEnv_ = accent ? 1.0f : 0.75f;
    clickPhase_ = 0.0;
    clickFreq_ = accent ? fAccent : fNormal;
  }

  double framesPerBeat() const noexcept {
    return (60.0 / bpm_) * sampleRate_;
  }

  double sampleRate_ = 48000.0;
  double bpm_ = 120.0;
  int tsNum_ = 4;
  int tsDen_ = 4;
  double spb_ = 0.0;
  double phase_ = 0.0;
  int beat_ = 0;
  uint32_t countInTotal_ = 0;
  uint32_t countInRemaining_ = 0;
  bool enabled_ = false;
  bool ticking_ = false;
  bool pendingClick_ = true;
  float volume_ = 0.9f;
  float clickEnv_ = 0.f;
  float clickPhase_ = 0.f;
  float clickFreq_ = 0.f;
  float hpPrev_ = 0.f;
  // 30 ms decay @ 48 kHz, overwritten by setSampleRate. A zero here would
  // silence every click, so it carries a valid default for engines that never
  // call setSampleRate (offline render before start()).
  float decay_ = 0.99930556f; // exp(-1 / (0.03 * 48000))
  float clickFreqNormal_ = 1500.f;  // Slightly brighter for more definition
  float clickFreqAccent_ = 3000.f;  // Brighter accent for clear distinction
};

} // namespace misuc::engine