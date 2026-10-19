#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <misuc/engine/audio/AudioBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/ControlStream.h>
#include <misuc/engine/core/RingBuffer.h>
#include <misuc/engine/dsp/ClipPlayer.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Eq.h>
#include <misuc/engine/dsp/Groove.h>
#include <misuc/engine/dsp/Metronome.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/dsp/Oscillator.h>
#include <misuc/engine/fx/BuiltinFx.h>
#include <misuc/engine/fx/Lv2Host.h>
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
  // Routing: -1 (default) sums straight to the master output; otherwise the
  // index of a bus in the same publish. A track on a bus is gated by the
  // track's own mute/solo first, then the bus applies its fader/pan/mute/solo.
  int busIndex = -1;
};

// One recorded take pulled out of the engine: the captured mono samples plus
// the exact transport span they occupy, so the clip a DAW builds from a take
// lands at the musical position where it was performed (correct even with a
// metronome count-in, which the engine never records).
struct RecordCapture {
  std::vector<float> samples;   // mono capture
  uint32_t sampleRate = 0;
  uint32_t channels = 0;        // 1 (mono fold of the live input)
  double startBeats = 0.0;      // transport position when capture began
  double endBeats = 0.0;        // transport position just past the last sample
  uint64_t framesLostTail = 0;  // input frames dropped at the kMaxRecordFrames
                                // ceiling (a longer-than-5-min single take)
  // Input-latency compensation actually applied to this take (in frames).
  // The capture start/end were advanced by this many frames (the input arrived
  // that late), so the clip lands where the performance happened (Phase Lat).
  int64_t captureOffsetFrames = 0;
};

// Real-time-safe per-bus parameters published together with the mixer tracks.
// The audio thread applies smoothed fader gain + constant-power pan, gates by
// mute/solo, and routes the bus into its `target` bus (or the master output
// when target < 0). The UI also publishes the bus's post-fader fx chain
// separately (publishFx); sidechain deps from that chain are folded into the
// render order so a key source is always processed before its consumer.
struct MixerBusParam {
  float gainDb = 0.0f; // -60..+12 (interpreted as linear gain)
  float pan = 0.0f;    // -1 = left, 0 = centre, +1 = right
  bool mute = false;
  bool solo = false;
  // Routing graph (Comp19): -1 = master output, else the index of another bus
  // in the same publish. Destination buses are processed after their sources.
  int target = -1;
};

// One parallel send (master spec §21 / Comp19): the source's post-fader signal
// is also accumulated, scaled by `level`, into the send's `target` bus while
// the source keeps its own route. `sourceTrack`/`sourceBus` carry engine
// indices; one is 0xFF (unset) and the other selects the source. The tap is
// post-fader, after the insert chain, and follows the source's mute/solo so a
// gated source sends silence.
struct MixerSendParam {
  uint8_t sourceTrack = 0xFF; // index into the published track params
  uint8_t sourceBus = 0xFF;   // index into the published bus params
  uint8_t target = 0xFF;      // target bus index
  float level = 0.f;          // linear gain (from levelDb)
  bool enabled = true;
};

// One automation curve for a track: points in beats, published with the same
// real-time-safe triple buffer as scores and the mixer. `param` names the
// target — "gain" or "volume" (linear amplitude) or "pan" (L/R position in
// [-1,+1], constant-power applied per sample), or an fx control-port target of
// the form "fx:<chainIndex>:<portIndex>" (the insert's position in the track's
// fx chain, plus the plugin control port number). The audio thread samples the
// curve against the transport position and piecewise-linearly interpolates per
// sample in process() and renderToWav, so automation is audible in both live
// and offline output. A curve with `active == false` (muted lane / no points)
// is held at its neutral value (gain 1.0 / pan 0.0).
struct AutomationParam {
  uint32_t trackIndex = 0;
  bool active = false;
  std::string param = "gain"; // "gain" | "volume" | "pan" | "fx:<chain>:<port>"
  std::vector<double> times;  // beats, sorted ascending
  std::vector<float> values;  // param units at the matching beat
};

// Parsed form of an "fx:<chainIndex>:<portIndex>" automation param string.
// `ok` is false for anything that is not an fx lane target.
struct FxAutomationParam {
  bool ok = false;
  uint32_t chainIndex = 0;
  uint32_t portIndex = 0;
};
inline FxAutomationParam parseFxAutomation(const std::string& param) noexcept {
  FxAutomationParam out;
  if (param.size() < 3 || param.compare(0, 3, "fx:") != 0)
    return out;
  const size_t sep = param.find(':', 3);
  if (sep == std::string::npos || sep == 3 || sep + 1 >= param.size())
    return out;
  auto toU32 = [](const std::string& s, uint32_t& dst) {
    if (s.empty() || s.size() > 9)
      return false;
    for (char c : s)
      if (c < '0' || c > '9')
        return false;
    dst = static_cast<uint32_t>(std::stoul(s));
    return true;
  };
  if (!toU32(param.substr(3, sep - 3), out.chainIndex))
    return out;
  if (!toU32(param.substr(sep + 1), out.portIndex))
    return out;
  out.ok = true;
  return out;
}

// The audio engine owns the transport, metronome, device test tone and master
// bus metering. It runs the real-time process loop through the backend. The UI
// drives it exclusively via lock-free control messages.
class AudioEngine : public AudioRenderer {
public:
  explicit AudioEngine(std::unique_ptr<AudioBackend> backend);
  ~AudioEngine() override;

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
  void setMetronome(bool enabled, float countInBeats, float volume = 0.9f);
  // Metronome click level (0..1) as last set on the audio thread.
  float metronomeVolume() const noexcept { return metronomeVolume_; }
  void setGroove(bool enabled);
  void setTestTone(bool on);
  void setMasterGainDb(float db);
  // Master brickwall peak limiter (default ON): instant-attack + exponential
  // release, ceiling -1 dBFS, applied after master gain and before the master
  // meters. Guarantees no output sample exceeds the ceiling. Live and offline
  // (renderToWav) rendering share the same stage.
  void setMasterLimiter(bool on);
  bool masterLimiterEnabled() const noexcept { return limiterOn_; }
  float masterLimiterGain() const noexcept { return limiter_.currentGain(); }
  void setMonitorGainDb(float db);
  // Loop region in beats (applied on the audio thread as transport frames).
  void setLoop(bool enabled, double startBeats, double endBeats);

  // Publish per-track mixer parameters (gain/pan/mute/solo) from any non-RT
  // thread. Applied by the audio thread via the same real-time-safe triple
  // buffer as the score/clips. A shorter vector keeps the remaining tracks at
  // their defaults (0 dB, centre, unmuted, un-soloed).
  void publishMixer(std::vector<MixerTrackParam> params);
  // Publish mixer tracks alongside their buses. Buses are indexed by the
  // tracks' `busIndex`; a shorter bus vector keeps the remaining buses at
  // their defaults (0 dB, centre, unmuted, un-soloed). `target` on a bus
  // routes it into another bus (rendering graph; -1 = master).
  void publishMixer(std::vector<MixerTrackParam> params,
                    std::vector<MixerBusParam> buses);
  // Publish mixer tracks, buses and parallel sends as one consistent snapshot
  // (Comp19): track/bus sends accumulate a post-fader tap into their target
  // bus while the source keeps its own route.
  void publishMixer(std::vector<MixerTrackParam> params,
                    std::vector<MixerBusParam> buses,
                    std::vector<MixerSendParam> sends);
  void clearMixer(); // reset every track and bus to the default mixer state

  // Publish per-track automation ("gain" lanes) from any non-RT thread.
  // Applied by the audio thread against the transport position: each curve is
  // sampled and piecewise-linearly interpolated per sample in process() and in
  // renderToWav, so the automation is audible in both live and offline output.
  // A curve with `active == false` (muted lane / no points) is held at unity.
  void publishAutomation(std::vector<AutomationParam> params);
  void clearAutomation();

  // Effect insert hosting -------------------------------------------------
  // Whether this build can host LV2 effects (lilv found at build time). Lazily
  // touches the host, so it is safe to call before any publish.
  bool fxSupported() noexcept;
  // Discover the hostable effect set (cached; control thread only): every LV2
  // plugin lilv offers plus the always-available built-in insert(s). Returns
  // the metadata needed to list effects and build a parameter panel. The
  // built-ins are present even when hosting is unavailable. Never touches the
  // audio thread.
  std::vector<FxPluginInfo> discoverFx();
  // Look up one discovered effect (built-in or hostable LV2), or nullptr when
  // unknown / not hostable.
  const FxPluginInfo* fxInfo(const std::string& uri) const;
  // Publish per-track post-fader effect chains from any non-RT thread. Each
  // insert becomes a freshly instantiated plugin instance created on the
  // caller's thread, handed to the audio thread via the same real-time-safe
  // triple buffer as scores and the mixer; the audio thread renders the enabled
  // instances in place per track, post-fader and pre-meter. A shorter chain
  // list keeps the remaining tracks' chains empty.
  void publishFx(std::vector<std::vector<FxInsertParam>> chains);
  // Publish per-track AND per-bus post-fader insert chains (Comp19). Bus
  // chains process a whole subgroup's summed signal at the bus stage; an
  // insert with a `sidechainBus` key feeds off that bus's post-fader output.
  // A shorter list keeps the remaining chains empty.
  void publishFx(std::vector<std::vector<FxInsertParam>> chains,
                 std::vector<std::vector<FxInsertParam>> busChains);
  void clearFx(); // publish empty chains (current instances survive one block)

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
  void previewSample(std::vector<float> interleaved, uint32_t channels,
                     float gain = 1.0f);

  // One-shot sample preview controls (RT-safe; call from any non-RT thread).
  // pause(true) holds the playback position so resume continues where it left
  // off; a new previewSample() always restarts from the top.
  void previewPause(bool paused);
  // Halt the armed one-shot preview immediately and rewind it to the top.
  void previewStop();
  // Set the linear gain (0..~2) applied to the armed preview on the audio
  // thread; the next block takes it into account.
  void previewGain(float gain);
  // Readbacks for the preview strip (polled only while a preview is active).
  bool previewActive() const noexcept;
  double previewPositionFrames() const noexcept;

  // Recording -------------------------------------------------------------
  // Capture the live audio input into an internal mono ring. startRecord()
  // allocates the (bounded) capture buffer off the real-time thread; capture
  // itself is RT-safe. Recording only accumulates while the transport plays,
  // so arming before a metronome count-in cannot capture the clicking beats.
  void startRecord();
  // Pushes the stop control and waits (a few ms) for the audio thread to
  // drain it, so takeRecord() never races a live write.
  void stopRecord();
  bool isRecording() const noexcept;
  // Copies the captured mono samples and their exact transport span since
  // startRecord(). Returns false while a live take is still recording or when
  // nothing was captured.
  bool takeRecord(RecordCapture& out) noexcept;
  static constexpr uint64_t kMaxRecordFrames = 48000ull * 300; // 5 min at 48k

  // Input-latency compensation (Phase Lat): when the capture stream reports
  // a device/buffer input delay, the take's start/end are advanced by that
  // many frames so the clip lands where the performance happened instead of
  // where the samples physically arrived. The offset applied to the most
  // recent take is observable by tests/UI.
  int64_t captureOffsetFrames() const noexcept;
  // Test hook: pin the latency source to a fixed input delay in ms instead of
  // asking the backend (which reports 0 when no stream is open). Use a
  // negative value to return to the backend-reported latency.
  void setInputLatencyMsOverride(double ms) {
    inputLatencyMsOverride_ = ms;
  }

  // Input monitoring: mix the live input into the output in real time
  // (playthrough) while it is also captured. ON by default so a record-ready
  // channel is immediately audible; the UI turns it off when no track is armed
  // (a disabled channel stays capture-only). Control-side; the audio thread
  // reads it with a relaxed atomic load per block.
  void setInputMonitor(bool on) noexcept { inputMonitor_.store(on); }
  bool inputMonitor() const noexcept { return inputMonitor_.load(); }

  // Export ----------------------------------------------------------------
  // Offline render of the currently published score + clips over the beat
  // range [fromBeats, toBeats) into an audio file. Mixing, gain, fades, mute,
  // loop-off, and master bus all flow through the live process() path so the
  // export matches what the user hears. Must be called with the audio stream
  // stopped. `format` selects the container (float WAV / 16-bit WAV / FLAC /
  // MP3 / OGG / AIFF) and `dither` folds a ±1 LSB TPDF into fixed-word-length
  // targets.
  // `exportSampleRate` (0 = the engine's native rate) selects the file's frame
  // rate; the mix is rendered at the native rate and resampled when that
  // differs (Phase R per-project export rate). `title`/`artist` are written as
  // the audio file's RIFF INFO (WAV) / Vorbis-comment (FLAC) / ID3 (MP3) tags.
  // `includeMetronome` lets the metronome tick into the bounce (default off).
  // `progress` (optional) is invoked with a 0..1 fraction after each chunk;
  // returning false cancels the render (no output file is produced).
  bool renderToFile(const std::string& path, double fromBeats, double toBeats,
                    uint32_t channels, misuc::audio::ExportFormat format,
                    bool dither, std::string& err,
                    uint32_t exportSampleRate = 0,
                    const std::string& title = std::string(),
                    const std::string& artist = std::string(),
                    bool includeMetronome = false,
                    const std::function<bool(double)>& progress = {});
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
  static constexpr uint32_t kMaxTrackCount = 128;
  // Cap on simultaneously published sends (per-source audio taps); the UI
  // publishes at most one send per (source, target) pair and honors this bound.
  static constexpr uint32_t kMaxSends = 256;

  // Per-bus post-gain meter snapshots (post fader + pan, before mute gating),
  // polled by the mixer UI. Mirrors the per-track snapshots above.
  float busMeterPeakL(uint32_t idx) const noexcept;
  float busMeterPeakR(uint32_t idx) const noexcept;
  float busMeterRmsL(uint32_t idx) const noexcept;
  float busMeterRmsR(uint32_t idx) const noexcept;
  bool busMeterClipped(uint32_t idx) const noexcept;

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
  std::atomic<bool> offlineRender_{false};
  // Set while an offline render is running with the "Include Metronome"
  // option: lets the metronome tick into the bounce even though offlineRender_
  // otherwise silences it.
  std::atomic<bool> renderIncludeMetronome_{false};
  float metronomeVolume_ = 0.9f;
  bool grooveEnabled_ = false;
  bool limiterOn_ = true;
  dsp::MasterLimiter limiter_;
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
  // Control-rate cadence for fx automation inside a render block: each segment
  // is subdivided into 64-frame control blocks so automation curves are sampled
  // (and applied to the inserts) at the segment's exact beats instead of once
  // at the segment start. 64 frames @48 kHz ≈ 750 Hz control rate.
  static constexpr uint32_t kFxControlFrames = 64;
  struct MixerTrackRT {
    core::Smoother gain; // linear gain target (from gainDb)
    core::Smoother panL; // constant-power left coefficient
    core::Smoother panR; // constant-power right coefficient
    dsp::ChannelMeter meterL;
    dsp::ChannelMeter meterR;
    bool mute = false;
    bool solo = false;
    int busIndex = -1; // -1 = master, else index into rtBuses_
  };
  std::array<MixerTrackRT, kMaxTrackCount> rtTracks_{};

  // Per-bus RT state (mirrors the track state; kMaxBusCount is the engine-wide
  // ceiling for named output groups).
  static constexpr uint32_t kMaxBusCount = 32;
  struct BusRT {
    core::Smoother gain;
    core::Smoother panL;
    core::Smoother panR;
    dsp::ChannelMeter meterL;
    dsp::ChannelMeter meterR;
    bool mute = false;
    bool solo = false;
    // Routing graph target (-1 = master); resolves a `Bus.target` to an index
    // once per published snapshot and drives the render order.
    int target = -1;
  };
  std::array<BusRT, kMaxBusCount> rtBuses_;

  // Per-block bus render order (compiled on the control side at fold time from
  // the bus targets + sidechain dependencies, so a bus is processed only after
  // every bus it feeds or keys on). Cycle edges are broken to the master.
  int busOrder_[kMaxBusCount];
  int busOrderLen_ = 0;

  // Per-send smoothed level (from levelDb). Indexed by the send's position in
  // the published send vector, so a send arriving at a new index just retargets
  // a smoother; taps tick it once per source sample.
  struct SendRT {
    core::Smoother level;
  };
  std::array<SendRT, kMaxSends> rtSends_{};

  // Per-track mixer slot triple buffer (published params, applied each block).
  struct MixerSlot {
    std::vector<MixerTrackParam> params;
    std::atomic<int> refs{0};
  };
  MixerSlot mixerSlots_[3];
  std::atomic<int> liveMixerSlot_{-1};
  int mixerPublishSlot_ = 0;

  // Per-bus slot triple buffer (paired with the mixer slot in each publish).
  struct BusSlot {
    std::vector<MixerBusParam> params;
    std::atomic<int> refs{0};
  };
  BusSlot busSlots_[3];
  std::atomic<int> liveBusSlot_{-1};
  int busPublishSlot_ = 0;

  // Per-source parallel sends (compiled on the control side from the Session's
  // `sends` list), published in the same triple buffer as the mixer/buses.
  struct SendSlot {
    std::vector<MixerSendParam> params;
    std::atomic<int> refs{0};
  };
  SendSlot sendSlots_[3];
  std::atomic<int> liveSendSlot_{-1};
  int sendPublishSlot_ = 0;

  // Per-track automation slot triple buffer (same RCU-free handshake as scores).
  struct AutomationSlot {
    std::vector<AutomationParam> params;
    std::atomic<int> refs{0};
  };
  AutomationSlot automationSlots_[3];
  std::atomic<int> liveAutomationSlot_{-1};
  int automationPublishSlot_ = 0;

  // Per-track post-fader effect chains, published through the same triple
  // buffer. The UI instantiates one plugin instance per enabled insert on its
  // own thread; the audio thread renders the enabled instances in place and
  // frees nothing (teardown happens on the control thread when a slot is
  // reused). Instances are either Lv2Host-owned (opaque lilv instance), the
  // engine's own always-available built-in EQ, or the sidechain-capable
  // built-in compressor, discriminated by Kind.
  struct FxPluginRT {
    enum class Kind : uint8_t { Lv2 = 0, BuiltinEq = 1, BuiltinComp = 2 };
    void* handle = nullptr; // Lv2Host / dsp::ParametricEq* / dsp::Compressor*
    Kind kind = Kind::Lv2;
    bool enabled = true;
    // Sidechain key source: index of the bus whose post-fader output feeds this
    // insert's key input (compressor "sidechainBus"), -1 = key its own signal.
    // Only meaningful on bus chains; track inserts never host sidechain.
    int scSource = -1;
  };
  struct FxSlot {
    std::vector<std::vector<FxPluginRT>> chains;    // per track
    std::vector<std::vector<FxPluginRT>> busChains; // per bus (Comp19)
    std::atomic<int> refs{0};
  };
  FxSlot fxSlots_[3];
  std::atomic<int> liveFxSlot_{-1};
  int fxPublishSlot_ = 0;
  std::unique_ptr<Lv2Host> fxHost_;
  Lv2Host* fxHost(); // lazy-create the host on first use

  // Per-track post-gain meter snapshots (audio thread stores, UI loads).
  std::array<std::atomic<float>, kMaxTrackCount> trackPeakL_{};
  std::array<std::atomic<float>, kMaxTrackCount> trackPeakR_{};
  std::array<std::atomic<float>, kMaxTrackCount> trackRmsL_{};
  std::array<std::atomic<float>, kMaxTrackCount> trackRmsR_{};
  std::array<std::atomic<bool>, kMaxTrackCount> trackClipped_{};

  // Per-bus post-gain meter snapshots (audio thread stores, UI loads).
  std::array<std::atomic<float>, kMaxBusCount> busPeakL_{};
  std::array<std::atomic<float>, kMaxBusCount> busPeakR_{};
  std::array<std::atomic<float>, kMaxBusCount> busRmsL_{};
  std::array<std::atomic<float>, kMaxBusCount> busRmsR_{};
  std::array<std::atomic<bool>, kMaxBusCount> busClipped_{};

  // Per-track stereo staging buffers for the block being mixed. Heap-owned so
  // the engine object (which tests and QA stack-allocate) stays small even at
  // kMaxTrackCount = 128; sized once in the ctor, never touched again by the
  // control thread while running.
  std::vector<float> scratchL_;
  std::vector<float> scratchR_;

  // Fixed per-bus stereo staging buffers for the block being mixed.
  std::vector<float> busScratchL_;
  std::vector<float> busScratchR_;

  // Audio input recording state (audio-thread owned once armed). Linear input
  // capture into a fixed buffer allocated on the control thread; the audio
  // thread only writes and never reallocates. `recordStartFrame_`/`recordLast-
  // WriteEnd_` pin the take's transport span (in frames since song start) so
  // the UI places the clip exactly where it was performed; latch + writes are
  // all done in the same RT fold, so they are consistent under loop wrap and
  // metronome count-in.
  bool recording_ = false;
  std::vector<float> recordBuffer_; // mono, kMaxRecordFrames floats
  uint64_t recordWritePos_ = 0;
  uint64_t recordStartFrame_ = 0;
  uint64_t recordLastWriteEnd_ = 0;
  bool recordStartLatched_ = false;
  uint64_t recordFramesLost_ = 0;
  // Input-latency compensation in frames applied to the capture latch (RT
  // thread, set via SetCaptureOffset control before record starts).
  int64_t captureOffsetFrames_ = 0;
  std::atomic<int64_t> captureOffsetApplied_{0};
  // Test override for the latency source (negative = use the backend's own
  // reported input latency).
  double inputLatencyMsOverride_ = -1.0;
  std::atomic<bool> inputMonitor_{true};
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
  bool previewPaused_ = false;
  float previewGain_ = 1.0f;
  uint64_t previewPosFrames_ = 0;
  // Published atomics so the UI preview strip can poll honest position/state.
  std::atomic<bool> previewActiveSnapshot_{false};
  std::atomic<uint64_t> previewPosSnapshot_{0};

  uint64_t lastProcessPosition_ = 0;
};

} // namespace misuc::engine