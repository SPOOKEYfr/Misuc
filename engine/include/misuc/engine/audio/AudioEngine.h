#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <misuc/engine/audio/AudioBackend.h>
#include <misuc/engine/core/ControlStream.h>
#include <misuc/engine/core/RingBuffer.h>
#include <misuc/engine/dsp/ClipPlayer.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Groove.h>
#include <misuc/engine/dsp/Metronome.h>
#include <misuc/engine/dsp/Midi.h>
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
  void setGroove(bool enabled);
  void setTestTone(bool on);
  void setMasterGainDb(float db);
  void setMonitorGainDb(float db);
  // Loop region in beats (applied on the audio thread as transport frames).
  void setLoop(bool enabled, double startBeats, double endBeats);

  // Publish a fresh MIDI playback score from any non-RT thread (e.g. when the
  // user edits notes). Handed to the audio thread via a real-time-safe
  // triple buffer; cheap enough to call on every committed edit.
  void setScore(std::vector<dsp::MidiNote> notes);
  void clearScore();

  // Publish decoded audio clips for playback (dsp::PlaybackClip built by
  // engine::buildClipScore). `storage` owns the interleaved sample data that
  // the clips reference; both are handed to the audio thread via the same
  // real-time-safe triple-buffer pattern as the score.
  void setClips(std::vector<dsp::PlaybackClip> clips, std::vector<float> storage);
  void clearClips();

  // Audition a note immediately (freq in Hz, velocity 0..1 as amplitude).
  // Sounds via the MIDI player's wall-clock preview path, even when the
  // transport is stopped.
  void previewNote(double freq, float velocity);

  // Recording -------------------------------------------------------------
  // Capture the live audio input into an internal mono ring. startRecord()
  // allocates the (bounded) capture buffer off the real-time thread; capture
  // itself is RT-safe. Recording only accumulates while the transport plays.
  void startRecord();
  // Pushes the stop control and waits (a few ms) for the audio thread to
  // drain it, so takeRecordedSamples() never races a live write.
  void stopRecord();
  bool isRecording() const noexcept;
  // Copies the captured mono samples captured since startRecord().
  bool takeRecordedSamples(std::vector<float>& out, uint32_t& sampleRate,
                           uint32_t& channels) noexcept;
  static constexpr uint64_t kMaxRecordFrames = 48000ull * 300; // 5 min at 48k

  // Export ----------------------------------------------------------------
  // Offline render of the currently published score + clips over the beat
  // range [fromBeats, toBeats) into a WAV file (float32). Mixing, gain, fades,
  // mute, loop-off, and master bus all flow through the live process() path so
  // the export matches what the user hears. Must be called with the audio
  // stream stopped.
  bool renderToWav(const std::string& path, double fromBeats, double toBeats,
                   uint32_t channels, std::string& err);

  // UI poll snapshots (loads atomics).
  bool isTransportPlaying() const noexcept;
  uint64_t playheadSamples() const noexcept;
  double sampleRate() const noexcept { return sampleRate_; }
  float meterPeakL() const noexcept;
  float meterPeakR() const noexcept;
  float meterRmsL() const noexcept;
  float meterRmsR() const noexcept;
  bool meterClipped() const noexcept;
  float inMeterPeak() const noexcept;
  float inMeterRms() const noexcept;
  bool inMeterClipped() const noexcept;
  uint64_t xruns() const noexcept;

  Transport& transport() noexcept { return transport_; }
  const Transport& transport() const noexcept { return transport_; }

  // AudioRenderer (real-time thread). Never allocates or blocks.
  void process(const float* input, uint32_t inputChannels, float* output,
               uint32_t outputChannels, uint32_t frames,
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
  dsp::Groove groove_;
  dsp::Oscillator tone_;
  PlayState state_ = PlayState::Stopped;
  bool countPending_ = false;
  float countInBeats_ = 0.f;
  bool testTone_ = false;
  bool metronomeEnabled_ = false;
  bool grooveEnabled_ = false;
  double toneFreq_ = 440.0;
  double sampleRate_ = 48000.0;
  core::Smoother masterSmoother_;
  core::Smoother monitorSmoother_;
  float monitorGain_ = 0.f;

  dsp::ChannelMeter meterL_;
  dsp::ChannelMeter meterR_;
  dsp::ChannelMeter inMeter_;
  core::MeterSnapshot snapshot_;

  // MIDI playback score + stateless polyphonic player. The audio thread holds
  // at most one slot (refs + 1) per block; the publisher round-robins three
  // slots and only writes a slot once its readers have left.
  struct ScoreSlot {
    std::vector<dsp::MidiNote> notes;
    std::atomic<int> refs{0};
  };
  ScoreSlot scoreSlots_[3];
  std::atomic<int> liveSlot_{-1};
  int publishSlot_ = 0;
  dsp::MidiPlayer midiPlayer_;

  // Audio clip playback slots, mirroring the score slots: the publisher fills
  // a free slot (owning the decoded samples in `storage`), the audio thread
  // holds at most one slot per block.
  struct ClipSlot {
    std::vector<float> storage;
    std::vector<dsp::PlaybackClip> clips;
    std::atomic<int> refs{0};
  };
  ClipSlot clipSlots_[3];
  std::atomic<int> liveClipSlot_{-1};
  int clipPublishSlot_ = 0;
  dsp::ClipPlayer clipPlayer_;

  // Audio input recording state (audio-thread owned once armed). Linear input
  // capture into a fixed buffer allocated on the control thread; the audio
  // thread only writes and never reallocates.
  bool recording_ = false;
  std::vector<float> recordBuffer_; // mono, kMaxRecordFrames floats
  uint64_t recordWritePos_ = 0;
  std::atomic<bool> recordingActive_{false};
  std::atomic<uint64_t> recordingFrames_{0};

  // Monotonic wall-clock frame counter (independent of transport) used for
  // note audition previews; incremented every process() block.
  uint64_t wallFrames_ = 0;

  uint64_t lastProcessPosition_ = 0;
};

} // namespace misuc::engine