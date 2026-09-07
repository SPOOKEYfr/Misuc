#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace misuc::dsp {

// A decoded audio region placed on the song timeline. Immutable snapshot; the
// audio thread only reads it. `samples` is channel-interleaved float data at
// the clip's native `sampleRate` owned by the publishing snapshot.
struct PlaybackClip {
  const float* samples = nullptr; // interleaved, owned by the caller
  uint32_t channels = 0;          // channels in `samples`
  uint32_t frames = 0;            // frames per channel
  double sampleRate = 0.0;        // source sample rate
  double startBeats = 0.0;        // placement on the timeline
  double offsetBeats = 0.0;       // trim-in offset into the source
  double durationBeats = 0.0;     // region length on the timeline
  float gain = 1.0f;              // clip's own gain (track gain applied by mix)
  double fadeInBeats = 0.0;
  double fadeOutBeats = 0.0;
  uint32_t track = 0;             // owning track index (into engine mixer array)
};

// Real-time-safe clip playback: resamples each decoded clip (linear
// interpolation) to the output rate and adds it to the master bus at its beat
// position. Pure function of absolute song time, so blocks stitch seamlessly
// and stop/rewind/loop restart cleanly — the same shape as dsp::MidiPlayer.
// No allocation, no locking.
class ClipPlayer {
public:
  void setSampleRate(double sr) noexcept {
    outputRate_ = sr > 0.0 ? sr : 48000.0;
  }
  void setTempo(double bpm) noexcept { bpm_ = bpm >= 20.0 ? bpm : 120.0; }
  void reset() noexcept {}

  // Add all clips overlapping the block [songFrame, songFrame+frames) into an
  // interleaved stereo pair (mono when stride < 2).
  void render(const std::vector<PlaybackClip>& clips, float* outL, float* outR,
              uint32_t frames, uint32_t stride, uint64_t songFrame) noexcept {
    if (frames == 0 || outL == nullptr)
      return;
    if (outR == nullptr)
      outR = outL;
    if (clips.empty())
      return;

    const double secPerBeat = 60.0 / bpm_;
    const double fpb = secPerBeat * outputRate_; // output frames per beat
    if (fpb <= 0.0)
      return;

    const double t0 = static_cast<double>(songFrame);
    const double t1 = t0 + static_cast<double>(frames);
    const double beats0 = t0 / fpb;
    const double beats1 = t1 / fpb;

    for (const PlaybackClip& c : clips) {
      if (c.gain <= 0.0f || c.frames == 0 || c.channels == 0 ||
          c.sampleRate <= 0.0)
        continue;
      if (c.durationBeats <= 0.0)
        continue;
      // Cheap block rejection against the clip's timeline span.
      if (beats1 <= c.startBeats || beats0 >= c.startBeats + c.durationBeats)
        continue;

      const double srcFramesPerBeat = c.sampleRate * secPerBeat;
      const uint32_t ch = c.channels;
      const uint32_t lastFrame = c.frames - 1;

      for (uint32_t n = 0; n < frames; ++n) {
        const double beats = (t0 + static_cast<double>(n)) / fpb;
        const double local = beats - c.startBeats - c.offsetBeats;
        if (local < 0.0)
          continue;
        if (local >= c.durationBeats)
          break; // clips are sorted; nothing later can overlap this block

        double env = 1.0;
        if (c.fadeInBeats > 0.0 && local < c.fadeInBeats)
          env = local / c.fadeInBeats;
        if (c.fadeOutBeats > 0.0 &&
            local > c.durationBeats - c.fadeOutBeats)
          env = std::min(env, (c.durationBeats - local) / c.fadeOutBeats);
        if (env <= 0.0)
          env = 0.0;

        const double pos = local * srcFramesPerBeat; // source frame (linear)
        if (pos >= static_cast<double>(c.frames))
          break;
        const double idxD = std::floor(pos);
        const double frac = pos - idxD;
        const uint32_t i0 = static_cast<uint32_t>(idxD);
        const uint32_t i1 = static_cast<uint32_t>(
            std::min<double>(idxD + 1.0, static_cast<double>(lastFrame)));

        const float g = c.gain * static_cast<float>(env);
        if (stride > 1) { // stereo output
          if (ch >= 2) {
            const float l0 = c.samples[static_cast<size_t>(i0) * ch];
            const float l1 = c.samples[static_cast<size_t>(i1) * ch];
            const float r0 = c.samples[static_cast<size_t>(i0) * ch + 1];
            const float r1 = c.samples[static_cast<size_t>(i1) * ch + 1];
            outL[n * stride] += static_cast<float>(l0 + (l1 - l0) * frac) * g;
            outR[n * stride] +=
                static_cast<float>(r0 + (r1 - r0) * frac) * g;
          } else { // mono source, both output channels
            const float a = c.samples[i0];
            const float b = c.samples[i1];
            const float v = static_cast<float>(a + (b - a) * frac) * g;
            outL[n * stride] += v;
            outR[n * stride] += v; // outR is already the channel-1 base
          }
        } else { // mono output: average the source channels
          float acc = 0.0f;
          for (uint32_t ci = 0; ci < ch; ++ci) {
            const float a =
                c.samples[static_cast<size_t>(i0) * ch + ci];
            const float b =
                c.samples[static_cast<size_t>(i1) * ch + ci];
            acc += static_cast<float>(a + (b - a) * frac);
          }
          outL[n] += acc / static_cast<float>(ch) * g;
        }
      }
    }
  }

private:
  double outputRate_ = 48000.0;
  double bpm_ = 120.0;
};

} // namespace misuc::dsp