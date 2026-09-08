#pragma once

#include <atomic>
#include <array>
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

// Real-time-safe per-track mixer parameters published by the UI thread. The
// audio thread applies smoothed gain, constant-power pan, and mute/solo gating
// per track at render time, so mixer changes land without re-scanning notes or
// clips (and without touching the real-time allocation budget).
struct MixerTrackParam {
  float gainDb = 0.0f; // -60..+12 (interpreted as linear gain)
  float pan = 0.0f;    // -1 = left, 0 = centre, +1 = right
  bool mute = false;
  bool solo = false;
};

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

  // Publish per-track mixer parameters (gain/pan/mute/solo) from any non-RT
  // thread. Applied by the audio thread via the same real-time-safe triple
  // buffer as the score/clips. A shorter vector keeps the remaining tracks at
  // their defaults (0 dB, centre, unmuted, un-soloed).
  void publishMixer(std::vector<MixerTrackParam> params);
  void clearMixer(); // reset every track to the default mixer state

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

  // Jump the transport playhead to `beat` (beats from the song start). The
  // transport keeps its current play/pause state: playing continues from the
  // new position, stopped/paused stay stopped with the caret at the new spot.
  // Applied on the audio thread; call from any non-RT thread.
  void seekToBeat(double beat);

  // Publish the effective end of the arrangement (beats). Normal (non-loop)
  // playback stops when the playhead reaches this beat, so an empty project
  // (end == 0) never plays and playback never runs past the content. Loop
  // playback is unaffected and spins its own range. Publish again whenever the
  // arrangement content changes. RT-safe; call from any non-RT thread.
  void setContentEnd(double endBeats);

  // Preview a whole sample (e.g. a browser audition or metronome click) as a
  // non-looping one-shot from the current wall clock. `interleaved` holds
  // `channels` (1 or 2) interleaved float samples at the engine's sample rate;
  // the engine publishes it RT-safely and arms it on the audio thread. The
  // transport does not need to be running for the preview to sound.
  void previewSample(std::vector<float> interleaved, uint32_t channels);

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

  // Per-track post-gain meter snapshots (post pan + fader, before mute/solo
  // gating) polled by the mixer UI. Loads atomics on the caller's thread.
  float trackMeterPeakL(uint32_t idx) const noexcept;
  float trackMeterPeakR(uint32_t idx) const noexcept;
  float trackMeterRmsL(uint32_t idx) const noexcept;
  float trackMeterRmsR(uint32_t idx) const noexcept;
  bool trackMeterClipped(uint32_t idx) const noexcept;
  static constexpr uint32_t kMaxTrackCount = 32;

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
  // Effective arrangement end in beats (UI-published). -1.0 means "not yet
  // published": normal playback then runs freely (the UI always publishes
  // before/at the first transport start). 0.0 means an empty arrangement,
  // which blocks playback start and stops immediately.
  double contentEndBeats_ = -1.0;
  core::Smoother masterSmoother_;
  core::Smoother monitorSmoother_;
  float monitorGain_ = 0.f;

  dsp::ChannelMeter meterL_;
  dsp::ChannelMeter meterR_;
  dsp::ChannelMeter inMeter_;
  core::MeterSnapshot snapshot_;

  // MIDI playback score + stateless polyphonic player. The audio thread holds
  // at most one slot (refs + 1) per block; the publisher round-robins three
  // slots and only writes a slot once its readers have left. Each slot stores
  // the score pre-split by track (partitioned on the publisher thread) so the
  // mixer can render a single track without touching the real-time path.
  struct ScoreSlot {
    std::vector<std::vector<dsp::MidiNote>> tracks;
    std::atomic<int> refs{0};
  };
  ScoreSlot scoreSlots_[3];
  std::atomic<int> liveSlot_{-1};
  int publishSlot_ = 0;
  dsp::MidiPlayer midiPlayer_;

  // Audio clip playback slots, mirroring the score slots: the publisher fills
  // a free slot (owning the decoded samples in `storage`; clips re-split by
  // track), the audio thread holds at most one slot per block.
  struct ClipSlot {
    std::vector<float> storage;
    std::vector<std::vector<dsp::PlaybackClip>> tracks;
    std::atomic<int> refs{0};
  };
  ClipSlot clipSlots_[3];
  std::atomic<int> liveClipSlot_{-1};
  int clipPublishSlot_ = 0;
  dsp::ClipPlayer clipPlayer_;

  // Per-track mixer state, audio-thread owned. Push-through smoothing avoids
  // zipper noise on fader/pan changes; the UI never touches these.
  static constexpr uint32_t kMaxBlockFrames = 8192;
  struct MixerTrackRT {
    core::Smoother gain; // linear gain target (from gainDb)
    core::Smoother panL; // constant-power left coefficient
    core::Smoother panR; // constant-power right coefficient
    dsp::ChannelMeter meterL;
    dsp::ChannelMeter meterR;
    bool mute = false;
    bool solo = false;
  };
  std::array<MixerTrackRT, kMaxTrackCount> rtTracks_{};

  // Per-track mixer slot triple buffer (published params, applied each block).
  struct MixerSlot {
    std::vector<MixerTrackParam> params;
    std::atomic<int> refs{0};
  };
  MixerSlot mixerSlots_[3];
  std::atomic<int> liveMixerSlot_{-1};
  int mixerPublishSlot_ = 0;

  // Per-track post-gain meter snapshots (audio thread stores, UI loads).
  std::array<std::atomic<float>, kMaxTrackCount> trackPeakL_{};
  std::array<std::atomic<float>, kMaxTrackCount> trackPeakR_{};
  std::array<std::atomic<float>, kMaxTrackCount> trackRmsL_{};
  std::array<std::atomic<float>, kMaxTrackCount> trackRmsR_{};
  std::array<std::atomic<bool>, kMaxTrackCount> trackClipped_{};

  // Fixed per-track stereo staging buffers for the block being mixed.
  float scratchL_[kMaxTrackCount * kMaxBlockFrames];
  float scratchR_[kMaxTrackCount * kMaxBlockFrames];

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

  // One-shot sample preview (browser audition / metronome accent). The control
  // thread publishes decoded samples through the same triple-buffer pattern as
  // scores, then arms them with a PreviewSample control message; the audio
  // thread renders the armed slot against the wall clock until consumed. The
  // data pointer is only dereferenced while the slot is held via refs.
  struct PreviewSlot {
    std::vector<float> samples; // interleaved, channels = 1 or 2
    std::atomic<int> refs{0};
  };
  PreviewSlot previewSlots_[3];
  std::atomic<int> livePreviewSlot_{-1};
  int previewPublishSlot_ = 0;
  // Audio-thread-owned one-shot preview state.
  bool previewActive_ = false;
  uint64_t previewStartWall_ = 0;
  uint32_t previewChannels_ = 2;

  uint64_t lastProcessPosition_ = 0;
};

} // namespace misuc::engine