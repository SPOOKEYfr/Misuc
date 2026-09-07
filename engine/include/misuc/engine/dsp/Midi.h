#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace misuc::dsp {

// A note in the playback score: absolute musical time plus a baked-in
// amplitude (velocity and per-track gain already applied). The score is an
// immutable snapshot published by the UI thread; the audio thread only reads
// it.
struct MidiNote {
  double startBeats = 0.0;
  double durationBeats = 1.0;
  double freq = 440.0; // Hz (from MIDI note number)
  float gain = 0.f;    // amplitude (velocity * track gain)
};

inline double midiNoteToFreq(double midiNote) noexcept {
  return 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
}

// Polyphonic MIDI playback for the transport. Every note overlapping the
// rendered block is synthesised as a pure function of absolute sample time:
// there is no persistent voice state, so successive blocks stitch seamlessly,
// stop/rewind/loop always restart cleanly, and the audio thread never
// allocates or blocks. The score lives in the caller (AudioEngine's real-time
// safe triple buffer) and is passed by const reference each block.
class MidiPlayer {
public:
  MidiPlayer() = default;

  void setSampleRate(double sr) noexcept { sampleRate_ = sr > 0 ? sr : 48000.0; }
  void setTempo(double bpm) noexcept { bpm_ = bpm >= 20.0 ? bpm : 120.0; }

  // Reset the internal scan cursor (called on play/stop/sample-rate change).
  void reset() noexcept {
    cursor_ = 0;
    lastScore_ = nullptr;
    lastT0_ = 0.0;
  }

  // Additive render into an interleaved stereo pair (mono if stride < 2).
  // `startFrame` is the transport position in frames at the block start.
  void render(const std::vector<MidiNote>& score, float* outL, float* outR,
              uint32_t frames, uint32_t stride,
              uint64_t startFrame) noexcept {
    if (frames == 0 || outL == nullptr || score.empty())
      return;
    const double fpb = (60.0 / bpm_) * sampleRate_;
    if (fpb <= 0.0)
      return;
    if (outR == nullptr)
      outR = outL;

    const double t0 = static_cast<double>(startFrame);
    const double t1 = t0 + static_cast<double>(frames);
    const double tail = kRelease * sampleRate_;

    // The score vector (and thus its content) changes when the CPU thread
    // publishes a new snapshot; detect by address and rescan from the start.
    const void* key = static_cast<const void*>(&score);
    if (key != lastScore_ || t0 < lastT0_) {
      cursor_ = 0;
      lastScore_ = key;
    }
    lastT0_ = t0;

    while (cursor_ < score.size() &&
           (score[cursor_].startBeats + score[cursor_].durationBeats) * fpb +
                   tail <=
               t0)
      ++cursor_;

    const double twoPi = 6.28318530717958647692;
    for (size_t i = cursor_; i < score.size(); ++i) {
      const MidiNote& n = score[i];
      const double nStart = n.startBeats * fpb;
      if (nStart >= t1)
        break; // sorted by startBeats
      const double nDur = n.durationBeats * fpb;
      if (nDur <= 0.0 || n.freq <= 0.0 || n.gain <= 0.f)
        continue;
      const double nEnd = nStart + nDur;

      const double lo = nStart > t0 ? nStart : t0;
      double hi = t1;
      if (nEnd + tail < hi)
        hi = nEnd + tail;
      if (hi <= lo)
        continue;

      const double freq = n.freq / sampleRate_; // cycles per frame
      const double attack = kAttack * sampleRate_;
      const double envEnd =
          1.0 - std::exp(-std::min(nDur, attack) / attack);
      const float g = n.gain * kLevel;

      const int64_t s0 = static_cast<int64_t>(std::ceil(lo));
      const int64_t s1 = static_cast<int64_t>(hi);
      const uint32_t base = static_cast<uint32_t>(static_cast<int64_t>(t0));
      for (int64_t s = s0; s < s1; ++s) {
        const double rel = static_cast<double>(s) - nStart;
        double env;
        if (s < static_cast<int64_t>(nEnd)) {
          env = 1.0 - std::exp(-rel / attack);
        } else {
          env = envEnd *
                std::exp(-(static_cast<double>(s) - nEnd) / tail);
        }
        const double p = freq * static_cast<double>(s);
        const double ph = p - std::floor(p);
        const double w = std::sin(twoPi * ph) +
                        0.5 * std::sin(2.0 * twoPi * ph) +
                        0.2 * std::sin(3.0 * twoPi * ph);
        const float v = static_cast<float>(w * env) * g;
        const uint32_t idx = static_cast<uint32_t>(s) - base;
        if (idx >= frames)
          continue;
        outL[idx * stride] += v;
        if (stride > 1)
          outR[idx * stride + 1] += v;
      }
    }
  }

private:
  static constexpr double kAttack = 0.010;  // seconds
  static constexpr double kRelease = 0.090; // seconds
  static constexpr float kLevel = 0.25f;

  double sampleRate_ = 48000.0;
  double bpm_ = 120.0;
  size_t cursor_ = 0;
  const void* lastScore_ = nullptr;
  double lastT0_ = 0.0;
};

} // namespace misuc::dsp