#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <string>

#include <misuc/engine/audio/AudioBackend.h>
#include <misuc/engine/core/ControlStream.h>
#include <misuc/engine/core/RingBuffer.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Metronome.h>
#include <misuc/engine/dsp/Oscillator.h>
#include <misuc/engine/transport/Transport.h>

namespace misuc::engine {

// Engine play states. Transport may be playing while the metronome counts in.
enum class PlayState : uint8_t { Stopped = 0, Counting = 1, Playing = 2 };

// The audio engine owns the transport, metronome, device test tone and master
// bus metering. It runs the real-time process loop through the backend. The UI
// drives it exclusively via lock-free control messages.
class AudioEngine : public AudioRenderer {
public:
  explicit AudioEngine(std::unique_ptr<AudioBackend> backend);
  ~AudioEngine() override = default;

  // Backend passthroughs.
  bool open(const AudioConfig& config, std::string& error);
  bool start();
  void closeAudio();
  bool isRunning() const;
  AudioBackend* backend() const noexcept { return backend_.get(); }
  BackendStatus backendStatus() const;
  LatencyInfo latency() const;

  // Control producers (any non-RT thread).
  void play(bool countIn);
  void stop(); // transport stop + rewind
  void pause();
  void setTempo(double bpm);
  void setMetronome(bool enabled, float countInBeats);
  void setTestTone(bool on);
  void setMasterGainDb(float db);

  // UI poll snapshots (loads atomics).
  bool isTransportPlaying() const noexcept;
  uint64_t playheadSamples() const noexcept;
  float meterPeakL() const noexcept;
  float meterPeakR() const noexcept;
  float meterRmsL() const noexcept;
  float meterRmsR() const noexcept;
  bool meterClipped() const noexcept;
  uint64_t xruns() const noexcept;

  Transport& transport() noexcept { return transport_; }
  const Transport& transport() const noexcept { return transport_; }

  // AudioRenderer (real-time thread). Never allocates or blocks.
  void process(float* data, uint32_t frames, uint32_t channels,
               uint64_t framePosition, double sampleRate) noexcept override;

private:
  void drainControls() noexcept;
  void beginPlay() noexcept;
  void renderTestTone(float* data, uint32_t frames, uint32_t channels) noexcept;

  std::unique_ptr<AudioBackend> backend_;

  // Lock-free control path between the UI thread and the audio thread.
  using ControlQueue = core::SPSCRing<ControlMessage, 1024>;
  std::unique_ptr<ControlQueue> controlQueue_;
  std::atomic<uint64_t> controlsDropped_{0};

  // Real-time state (only mutated on the audio thread).
  Transport transport_;
  Metronome metronome_;
  dsp::Oscillator tone_;
  PlayState state_ = PlayState::Stopped;
  bool countPending_ = false;
  float countInBeats_ = 0.f;
  bool testTone_ = false;
  bool metronomeEnabled_ = false;
  double toneFreq_ = 440.0;
  double sampleRate_ = 48000.0;
  core::Smoother masterSmoother_;

  dsp::ChannelMeter meterL_;
  dsp::ChannelMeter meterR_;
  core::MeterSnapshot snapshot_;

  uint64_t lastProcessPosition_ = 0;
};

} // namespace misuc::engine