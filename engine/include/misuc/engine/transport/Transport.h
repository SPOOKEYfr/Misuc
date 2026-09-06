#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace misuc::engine {

// Musical transport: BPM, time signature, position, looping. Pure math with a
// thin atomic wall clock so the UI can poll position thread-safely.
class Transport {
public:
  void reset() noexcept { position_ = 0; }

  void setSampleRate(double sr) noexcept {
    sampleRate_ = sr > 0.0 ? sr : 48000.0;
  }

  void setTempo(double bpm) noexcept {
    bpm_ = bpm < 20.0 ? 20.0 : (bpm > 400.0 ? 400.0 : bpm);
  }
  double tempo() const noexcept { return bpm_; }

  void setTimeSignature(int num, int den) noexcept {
    tsNum_ = num > 0 ? num : 4;
    tsDen_ = den > 0 ? den : 4;
  }

  double framesPerBeat() const noexcept {
    return (60.0 / bpm_) * sampleRate_;
  }
  double framesPerBar() const noexcept { return framesPerBeat() * tsNum_; }

  void play() noexcept { playing_.store(true); }
  void pause() noexcept { playing_.store(false); }
  void stop() noexcept { playing_.store(false); position_ = 0; }
  bool isPlaying() const noexcept { return playing_.load(); }

  void setPosition(uint64_t frames) noexcept { position_ = frames; }
  uint64_t positionFrames() const noexcept { return position_; }
  double positionSeconds() const noexcept { return position_ / sampleRate_; }

  void setLooping(bool on) noexcept { looping_.store(on); }
  bool isLooping() const noexcept { return looping_.load(); }
  void setLoopRange(uint64_t start, uint64_t end) noexcept {
    loopStart_ = start;
    loopEnd_ = end > start ? end : start + 1;
  }
  uint64_t loopStart() const noexcept { return loopStart_; }
  uint64_t loopEnd() const noexcept { return loopEnd_; }

  // Advance playhead by up to nFrames, honouring loop wrap. Returns the number
  // of frames actually advanced (may be 0 if paused).
  uint64_t advance(uint64_t nFrames) noexcept {
    if (!playing_.load())
      return 0;
    position_ += nFrames;
    if (looping_.load() && loopEnd_ > loopStart_ && position_ >= loopEnd_) {
      const uint64_t range = loopEnd_ - loopStart_;
      if (range > 0)
        position_ = loopStart_ + (position_ - loopStart_) % range;
    }
    return nFrames;
  }

  // Bars/beats position for a given frame (bar is 1-based per DAW convention).
  void barsBeats(uint64_t frame, int& bar, int& beat, double& beatFraction) const noexcept {
    const double beats = static_cast<double>(frame) / framesPerBeat();
    const double barF = std::floor(beats / tsNum_);
    const double beatF = beats - barF * tsNum_;
    bar = static_cast<int>(barF) + 1;
    beat = static_cast<int>(std::floor(beatF)) + 1;
    beatFraction = beatF - std::floor(beatF);
  }

  // "bar.beat.sixteenth.subtick" e.g. "3.2.1.032" (bar & beat 1-based)
  void formatPosition(uint64_t frame, char* buf, size_t n) const noexcept {
    const double fpb = framesPerBeat();
    const double beats = static_cast<double>(frame) / fpb;
    const double barF = std::floor(beats / tsNum_);
    const double beatInBar = beats - barF * tsNum_;
    const double sixteenthF = beatInBar * 4.0;
    const int sixteenth = static_cast<int>(sixteenthF);
    const int sub = static_cast<int>((sixteenthF - sixteenth) * 240.0);
    std::snprintf(buf, n, "%d.%d.%d.%03d",
                  static_cast<int>(barF) + 1,
                  static_cast<int>(std::floor(beatInBar)) + 1,
                  (sixteenth % 4) + 1,
                  sub);
  }

  int timeSigNumerator() const noexcept { return tsNum_; }
  int timeSigDenominator() const noexcept { return tsDen_; }

private:
  std::atomic<bool> playing_{false};
  std::atomic<bool> looping_{false};
  double sampleRate_ = 48000.0;
  double bpm_ = 120.0;
  int tsNum_ = 4;
  int tsDen_ = 4;
  uint64_t position_ = 0;
  uint64_t loopStart_ = 0;
  uint64_t loopEnd_ = 0;
};

} // namespace misuc::engine