#pragma once

#include <cmath>
#include <cstdint>

namespace misuc::dsp {

// A self-contained, deterministic drum + bass groove. It triggers kick, snare,
// hats and a bass note quantized to the musical grid, so the transport has
// audible output while playback runs. All state is envelope/phase internal;
// render() is realtime-safe (no allocation, no blocking).
class Groove {
public:
  Groove() = default;

  void setSampleRate(double sr) { sampleRate_ = sr > 0 ? sr : 48000.0; }
  void setTempo(double bpm) { tempo_ = bpm >= 20.0 ? bpm : 120.0; }
  void setTimeSignature(int num, int den) {
    tsNum_ = num > 0 ? num : 4;
    tsDen_ = den > 0 ? den : 4;
  }
  void setEnabled(bool on) { enabled_ = on; }
  void reset() { bassPhase_ = 0.0; }

  // Renders into an interleaved stereo buffer (mono if stride < 2). `startFrame`
  // is the transport position (frames since playback start) at the block start.
  void render(float* outL, float* outR, uint32_t frames, uint32_t stride,
              uint64_t startFrame) {
    if (!enabled_ || frames == 0)
      return;
    if (outR == nullptr)
      outR = outL;

    const double beatFrames =
        (60.0 / tempo_) * sampleRate_ * 4.0 / static_cast<double>(tsDen_);
    if (beatFrames <= 0.0)
      return;
    const double sixteenthFrames = beatFrames / 4.0;
    const int sixteenthsPerBar = 16; // musical grid: 16 sixteenths per bar

    const double pos = static_cast<double>(startFrame);
    const double blockEnd = static_cast<double>(startFrame) + frames;

    // Battery of one-shot voices. Envelopes are recomputed from scratch for
    // each hit, so there is no per-voice persistent state to unwind on stop.
    const int firstSlot =
        static_cast<int>(std::floor(pos / sixteenthFrames + 1e-9));
    for (int slot = firstSlot;; ++slot) {
      const double slotStart = static_cast<double>(slot) * sixteenthFrames;
      if (slotStart >= blockEnd)
        break;
      if (slotStart + 1e-9 < pos)
        continue;

      const int slotInBar = slot % sixteenthsPerBar; // 0..15
      const int beat = slotInBar / 4;                // beat within bar
      const bool isBeat = slotInBar % 4 == 0;
      const bool isQuarter = isBeat;
      const bool isHalf = isBeat && (beat % 2 == 0);   // 1st/3rd beat
      const bool isEighth = slotInBar % 2 == 0;

      const uint32_t offset =
          static_cast<uint32_t>(std::round(slotStart - pos));
      if (isQuarter)
        renderKick(outL, outR, frames, stride, offset);
      if (beat == 1 || beat == 3) // 2nd and 4th beat
        renderSnare(outL, outR, frames, stride, offset);
      if (isEighth)
        renderHat(outL, outR, frames, stride, offset);
      if (slotInBar == 0)
        renderBass(outL, outR, frames, stride, offset);
    }
  }

private:
  double kickEnv(double t) const {
    return std::exp(-t / 0.050);
  }
  double kickFrq(double t) const { return 55.0 + 95.0 * std::exp(-t / 0.012); }

  void renderKick(float* outL, float* outR, uint32_t frames, uint32_t stride,
                  uint32_t offset) {
    for (uint32_t i = offset; i < frames; ++i) {
      const double t = static_cast<double>(i - offset) / sampleRate_;
      const double f = kickFrq(t) * 2.0 * M_PI;
      phase_ += f / sampleRate_;
      const double s = std::sin(phase_) * kickEnv(t) * 0.9;
      outL[i * stride] += static_cast<float>(s);
      if (stride > 1)
        outR[i * stride + 1] += static_cast<float>(s);
    }
  }

  void renderSnare(float* outL, float* outR, uint32_t frames, uint32_t stride,
                   uint32_t offset) {
    uint32_t seed = static_cast<uint32_t>(offset + 1);
    for (uint32_t i = offset; i < frames; ++i) {
      const double t = static_cast<double>(i - offset) / sampleRate_;
      // Cheap deterministc-ish noise (xorshift).
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      const double n = static_cast<double>(seed) / 4294967295.0 * 2.0 - 1.0;
      const double body = std::sin(i * 0.5) * std::exp(-t / 0.06);
      const double s = (n * 0.8 + body * 0.25) * std::exp(-t / 0.070) * 0.7;
      outL[i * stride] += static_cast<float>(s);
      if (stride > 1)
        outR[i * stride + 1] += static_cast<float>(s);
    }
  }

  void renderHat(float* outL, float* outR, uint32_t frames, uint32_t stride,
                 uint32_t offset) {
    uint32_t seed = static_cast<uint32_t>(offset * 2654435761u + 1u);
    double prev = 0.0;
    for (uint32_t i = offset; i < frames; ++i) {
      const double t = static_cast<double>(i - offset) / sampleRate_;
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      const double n = static_cast<double>(seed) / 4294967295.0 * 2.0 - 1.0;
      const double hp = n - prev; // first-difference high-pass
      prev = n;
      const double s = hp * std::exp(-t / 0.018) * 0.32;
      outL[i * stride] += static_cast<float>(s);
      if (stride > 1)
        outR[i * stride + 1] += static_cast<float>(s);
    }
  }

  // Simple cycling minor-ish bassline, one note per bar.
  void renderBass(float* outL, float* outR, uint32_t frames, uint32_t stride,
                  uint32_t offset) {
    static constexpr double kNotes[4] = {55.0, 65.41, 49.0, 43.65};
    const double f = kNotes[barCount_ % 4];
    for (uint32_t i = offset; i < frames; ++i) {
      const double t = static_cast<double>(i - offset) / sampleRate_;
      bassPhase_ += f / sampleRate_;
      double s = std::sin(bassPhase_);
      s += 0.4 * std::sin(2.0 * bassPhase_);
      s *= 0.55 * (1.0 - std::exp(-t / 0.012)); // soft attack
      s *= 0.75;
      outL[i * stride] += static_cast<float>(s);
      if (stride > 1)
        outR[i * stride + 1] += static_cast<float>(s);
    }
    if (offset == 0)
      ++barCount_;
  }

  double sampleRate_ = 48000.0;
  double tempo_ = 120.0;
  int tsNum_ = 4, tsDen_ = 4;
  bool enabled_ = false;
  double phase_ = 0.0;
  double bassPhase_ = 0.0;
  uint32_t barCount_ = 0;
};

} // namespace misuc::dsp