#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace misuc::dsp {

// A note in the playback score: absolute musical time plus a baked-in
// amplitude of just the note's velocity (per-track gain, pan, mute and solo
// are applied by the mixer in the audio thread, keyed by `track`). The score
// is an immutable snapshot published by the UI thread; the audio thread only
// reads it.
struct MidiNote {
  double startBeats = 0.0;
  double durationBeats = 1.0;
  double freq = 440.0; // Hz (from MIDI note number)
  float gain = 0.f;    // amplitude (velocity / 127)
  uint32_t track = 0;  // owning track index (into the engine mixer array)
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
//
// Two independent timebases are rendered: the *song* timebase (score notes,
// locked to the transport, only when `includeScore` is set) and the *wall*
// timebase (a monotonic frame clock used for short audition previews, which
// sound even while the transport is stopped).
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
    for (Preview& p : previews_) {
      p.dur = 0.0; // mark empty
      p.freq = 0.0;
      p.gain = 0.f;
    }
  }

  // Audition a note immediately (RT-safe). The trigger time must be expressed
  // in the same wall-time frames passed to render(). A fixed-size ring of
  // preview slots means an unchecked flood of triggers overwrites the oldest.
  void triggerPreview(double startFrame, double durFrames, double freq,
                      float gain) noexcept {
    Preview& p = previews_[previewNext_];
    previewNext_ = (previewNext_ + 1) % kPreviewSlots;
    p.start = startFrame;
    p.dur = durFrames;
    p.freq = freq;
    p.gain = gain;
  }

  // Additive render into an interleaved stereo pair (mono if stride < 2).
  // `songFrame` is the transport position in frames at the block start;
  // `wallFrame` is the equivalent frame count of the wall clock. This
  // combined entry point renders the score (when `includeScore`) and the
  // audition preview ring, mixing both into the output — used by the engine
  // as a convenience and by the unit tests.
  void render(const std::vector<MidiNote>& score, bool includeScore,
              float* outL, float* outR, uint32_t frames, uint32_t stride,
              uint64_t songFrame, uint64_t wallFrame) noexcept {
    renderScore(score, includeScore, outL, outR, frames, stride, songFrame);
    renderPreviews(outL, outR, frames, stride, wallFrame);
  }

  // Render only the score notes (no previews). Used per-track by the mixer.
  void renderScore(const std::vector<MidiNote>& score, bool includeScore,
                   float* outL, float* outR, uint32_t frames, uint32_t stride,
                   uint64_t songFrame) noexcept {
    if (frames == 0 || outL == nullptr)
      return;
    if (outR == nullptr)
      outR = outL;

    const double fpb = (60.0 / bpm_) * sampleRate_;

    if (includeScore && !score.empty() && fpb > 0.0) {
      const double t0 = static_cast<double>(songFrame);
      const double t1 = t0 + static_cast<double>(frames);

      // The score vector (and thus its content) changes when the CPU thread
      // publishes a new snapshot; detect by address and rescan from the start.
      const void* key = static_cast<const void*>(&score);
      if (key != lastScore_ || t0 < lastT0_) {
        cursor_ = 0;
        lastScore_ = key;
      }
      lastT0_ = t0;

      const double tail = kRelease * sampleRate_;
      while (cursor_ < score.size() &&
             (score[cursor_].startBeats + score[cursor_].durationBeats) * fpb +
                     tail <=
                 t0)
        ++cursor_;

      for (size_t i = cursor_; i < score.size(); ++i) {
        const MidiNote& n = score[i];
        const double nStart = n.startBeats * fpb;
        if (nStart >= t1)
          break; // sorted by startBeats
        const double nDur = n.durationBeats * fpb;
        if (nDur <= 0.0 || n.freq <= 0.0 || n.gain <= 0.f)
          continue;
        addNoteBlock(outL, outR, frames, stride, t0, nStart, nStart + nDur,
                     kAttack * sampleRate_, tail, n.freq, n.gain);
      }
    }
  }

  // Render only the short wall-clock audition previews (no score). Called once
  // by the mixer so previews aren't duplicated across per-track renders.
  void renderPreviews(float* outL, float* outR, uint32_t frames,
                      uint32_t stride, uint64_t wallFrame) noexcept {
    if (frames == 0 || outL == nullptr)
      return;
    if (outR == nullptr)
      outR = outL;
    for (const Preview& p : previews_) {
      if (p.dur <= 0.0 || p.freq <= 0.0 || p.gain <= 0.f)
        continue;
      addNoteBlock(outL, outR, frames, stride, static_cast<double>(wallFrame),
                   p.start, p.start + p.dur, kPreviewAttack * sampleRate_,
                   kPreviewRelease * sampleRate_, p.freq, p.gain);
    }
  }

private:
  struct Preview {
    double start = 0.0;
    double dur = 0.0;
    double freq = 0.0;
    float gain = 0.f;
  };

  // Shared per-note synthesis: pure function of the absolute sample position,
  // so it is block- and copy-independent. `baseFrame` is the current block's
  // start in the relevant timebase (song or wall).
  void addNoteBlock(float* outL, float* outR, uint32_t frames, uint32_t stride,
                    double baseFrame, double nStart, double nEnd,
                    double attack, double tail, double freq,
                    float gain) const noexcept {
    const double t1 = baseFrame + static_cast<double>(frames);
    double lo = nStart > baseFrame ? nStart : baseFrame;
    double hi = t1;
    if (nEnd + tail < hi)
      hi = nEnd + tail;
    if (hi <= lo)
      return;

    const double envEnd =
        1.0 - std::exp(-std::min(nEnd - nStart, attack) / attack);
    const float g = gain * kLevel;
    const int64_t s0 = static_cast<int64_t>(std::ceil(lo));
    const int64_t s1 = static_cast<int64_t>(hi);
    const int64_t baseI = static_cast<int64_t>(baseFrame);
    const double twoPi = 6.28318530717958647692;
    for (int64_t s = s0; s < s1; ++s) {
      const double rel = static_cast<double>(s) - nStart;
      double env;
      if (s < static_cast<int64_t>(nEnd)) {
        env = 1.0 - std::exp(-rel / attack);
      } else {
        env = envEnd * std::exp(-(static_cast<double>(s) - nEnd) / tail);
      }
      const double p = freq * rel / sampleRate_; // cycles elapsed
      const double ph = p - std::floor(p);
      const double w = std::sin(twoPi * ph) + 0.5 * std::sin(2.0 * twoPi * ph) +
                       0.2 * std::sin(3.0 * twoPi * ph);
      const float v = static_cast<float>(w * env) * g;
      const uint32_t idx = static_cast<uint32_t>(s - baseI);
      if (idx >= frames)
        continue;
      outL[idx * stride] += v;
      if (stride > 1)
        outR[idx * stride + 1] += v;
    }
  }

  static constexpr double kAttack = 0.010;  // seconds (score notes)
  static constexpr double kRelease = 0.090; // seconds (score notes)
  static constexpr double kPreviewAttack = 0.003;
  static constexpr double kPreviewRelease = 0.030;
  static constexpr float kLevel = 0.25f;
  static constexpr size_t kPreviewSlots = 8;

  double sampleRate_ = 48000.0;
  double bpm_ = 120.0;
  size_t cursor_ = 0;
  const void* lastScore_ = nullptr;
  double lastT0_ = 0.0;
  Preview previews_[kPreviewSlots];
  size_t previewNext_ = 0;
};

} // namespace misuc::dsp