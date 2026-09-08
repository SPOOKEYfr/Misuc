#include <misuc/engine/audio/AudioEngine.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/Logger.h>

using misuc::core::logInfo;

namespace misuc::engine {

AudioEngine::AudioEngine(std::unique_ptr<AudioBackend> backend)
    : backend_(std::move(backend ? std::move(backend)
                                 : std::make_unique<PipeWireBackend>())),
      controlQueue_(std::make_unique<ControlQueue>()) {
  // ~5 ms fade coefficient at 48 kHz.
  masterSmoother_.setCoefficient(0.004f);
  monitorSmoother_.setCoefficient(0.004f);
  meterL_.setDecaySeconds(0.35f, 0.30f);
  meterR_.setDecaySeconds(0.35f, 0.30f);
  inMeter_.setDecaySeconds(0.35f, 0.30f);
  setMasterGainDb(0.f);
  setMonitorGainDb(0.f);
  clipPlayer_.setSampleRate(sampleRate_);
  clipPlayer_.setTempo(transport_.tempo());
  for (uint32_t i = 0; i < kMaxTrackCount; ++i) {
    MixerTrackRT& rt = rtTracks_[i];
    rt.gain.setCoefficient(0.004f);
    rt.panL.setCoefficient(0.004f);
    rt.panR.setCoefficient(0.004f);
    rt.gain.reset(1.f);
    rt.panL.reset(0.707106781f); // constant-power centre
    rt.panR.reset(0.707106781f);
    rt.meterL.setSampleRate(static_cast<float>(sampleRate_));
    rt.meterR.setSampleRate(static_cast<float>(sampleRate_));
    rt.meterL.setDecaySeconds(0.35f, 0.30f);
    rt.meterR.setDecaySeconds(0.35f, 0.30f);
  }
}

bool AudioEngine::open(const AudioConfig& config, std::string& error) {
  return backend_->open(config, error);
}

bool AudioEngine::start() {
  // Reset real-time state once per session.
  transport_.setSampleRate(sampleRate_);
  transport_.setPosition(0);
  transport_.pause();
  state_ = PlayState::Stopped;
  countPending_ = false;
  metronome_.reset();
  metronome_.setTicking(false);
  metronome_.setSampleRate(sampleRate_);
  metronome_.setTempo(transport_.tempo());
  metronome_.setTimeSignature(transport_.timeSigNumerator(),
                              transport_.timeSigDenominator());
  groove_.setSampleRate(sampleRate_);
  groove_.setTempo(transport_.tempo());
  groove_.setTimeSignature(transport_.timeSigNumerator(),
                           transport_.timeSigDenominator());
  groove_.setEnabled(grooveEnabled_);
  groove_.reset();
  tone_.setSampleRate(sampleRate_);
  tone_.setFrequency(toneFreq_);
  tone_.reset();
  tone_.setWaveform(dsp::Waveform::Sine);
  midiPlayer_.setSampleRate(sampleRate_);
  midiPlayer_.setTempo(transport_.tempo());
  midiPlayer_.reset();
  clipPlayer_.setSampleRate(sampleRate_);
  clipPlayer_.setTempo(transport_.tempo());
  clipPlayer_.reset();
  lastProcessPosition_ = 0;
  snapshot_.clear();
  for (uint32_t i = 0; i < kMaxTrackCount; ++i) {
    MixerTrackRT& rt = rtTracks_[i];
    rt.meterL.clearClip();
    rt.meterR.clearClip();
    rt.gain.reset(1.f);
    rt.panL.reset(0.707106781f);
    rt.panR.reset(0.707106781f);
    rt.mute = false;
    rt.solo = false;
    trackPeakL_[i].store(0.f, std::memory_order_relaxed);
    trackPeakR_[i].store(0.f, std::memory_order_relaxed);
    trackRmsL_[i].store(0.f, std::memory_order_relaxed);
    trackRmsR_[i].store(0.f, std::memory_order_relaxed);
    trackClipped_[i].store(false, std::memory_order_relaxed);
  }

  const bool ok = backend_->start(*this);
  if (!ok)
    return false;
  logInfo("Audio engine started on '%s' at %u Hz, %u-frame buffers.",
          backend_->backendName().c_str(),
          backendStatus().negotiatedRate, backendStatus().negotiatedBuffer);
  return true;
}

void AudioEngine::closeAudio() { backend_->stop(); }

bool AudioEngine::isRunning() const { return backend_->isRunning(); }

BackendStatus AudioEngine::backendStatus() const { return backend_->status(); }

LatencyInfo AudioEngine::latency() const { return backend_->latency(); }

void AudioEngine::play(bool countIn) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Transport);
  m.f0 = static_cast<float>(TransportAction::Play);
  m.f1 = countIn ? 1.f : 0.f;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::stop() {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Transport);
  m.f0 = static_cast<float>(TransportAction::Stop);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::pause() {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Transport);
  m.f0 = static_cast<float>(TransportAction::Pause);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setTempo(double bpm) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::SetTempo);
  m.d0 = bpm;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setMetronome(bool enabled, float countInBeats) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Metronome);
  m.f0 = enabled ? 1.f : 0.f;
  m.f1 = countInBeats;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setGroove(bool enabled) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Groove);
  m.f0 = enabled ? 1.f : 0.f;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setLoop(bool enabled, double startBeats, double endBeats) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Loop);
  m.f0 = enabled ? 1.f : 0.f;
  m.d0 = startBeats;
  m.d1 = endBeats;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setMonitorGainDb(float db) {
  monitorGain_ = dsp::gainFromDb(db);
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::MonitorGain);
  m.f0 = dsp::gainFromDb(db);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setTestTone(bool on) {
  testTone_ = on;
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::TestTone);
  m.f0 = on ? 1.f : 0.f;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setMasterGainDb(float db) {
  masterSmoother_.setTarget(dsp::gainFromDb(db));
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::MasterGain);
  m.f0 = dsp::gainFromDb(db);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setScore(std::vector<dsp::MidiNote> notes) {
  const int c = publishSlot_;
  publishSlot_ = (publishSlot_ + 1) % 3;
  ScoreSlot& slot = scoreSlots_[c];
  // Wait until the audio thread (if it holds this slot) is done with it. The
  // audio thread only ever holds one slot for the duration of one process()
  // block, so this spin is bounded and effectively never waits.
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  // Pre-split into per-track vectors so the audio thread can mix one track
  // without any real-time allocation.
  std::vector<std::vector<dsp::MidiNote>> byTrack(kMaxTrackCount);
  for (dsp::MidiNote& n : notes)
    if (n.track < kMaxTrackCount)
      byTrack[n.track].push_back(std::move(n));
  slot.tracks = std::move(byTrack);
  std::atomic_thread_fence(std::memory_order_release);
  liveSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearScore() {
  setScore(std::vector<dsp::MidiNote>());
}

void AudioEngine::setClips(std::vector<dsp::PlaybackClip> clips,
                           std::vector<float> storage) {
  const int c = clipPublishSlot_;
  clipPublishSlot_ = (clipPublishSlot_ + 1) % 3;
  ClipSlot& slot = clipSlots_[c];
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();

  // The clip's sample pointers target the incoming `storage`; move ownership
  // first (the buffer address is unchanged by the move), rebase explicitly,
  // then re-split by track for the mixer. Reviewer note: sample pointers stay
  // valid because a vector move hands the same heap allocation to the slot.
  slot.storage = std::move(storage);
  const float* base = slot.storage.empty() ? nullptr : slot.storage.data();
  std::vector<std::vector<dsp::PlaybackClip>> byTrack(kMaxTrackCount);
  for (dsp::PlaybackClip& cl : clips) {
    if (cl.track >= kMaxTrackCount)
      continue;
    if (cl.samples && base)
      cl.samples =
          base + static_cast<ptrdiff_t>(cl.samples - slot.storage.data());
    byTrack[cl.track].push_back(std::move(cl));
  }
  slot.tracks = std::move(byTrack);
  std::atomic_thread_fence(std::memory_order_release);
  liveClipSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearClips() {
  setClips(std::vector<dsp::PlaybackClip>(), std::vector<float>());
}

void AudioEngine::publishMixer(std::vector<MixerTrackParam> params) {
  if (params.size() > kMaxTrackCount)
    params.resize(kMaxTrackCount);
  const int c = mixerPublishSlot_;
  mixerPublishSlot_ = (mixerPublishSlot_ + 1) % 3;
  MixerSlot& slot = mixerSlots_[c];
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  slot.params = std::move(params);
  std::atomic_thread_fence(std::memory_order_release);
  liveMixerSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearMixer() {
  publishMixer(std::vector<MixerTrackParam>());
}

void AudioEngine::startRecord() {
  // Allocate outside the real-time thread; the audio thread only writes.
  recordBuffer_.assign(static_cast<size_t>(kMaxRecordFrames), 0.f);
  recordWritePos_ = 0;
  recordingFrames_.store(0, std::memory_order_relaxed);
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Record);
  m.f0 = 1.f;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::stopRecord() {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Record);
  m.f0 = 0.f;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
  // Wait for the audio thread to drain the stop control (a few ms at most).
  // With no live stream (headless/offline use) the caller drains controls
  // explicitly, so don't block.
  if (!isRunning())
    return;
  for (int i = 0; i < 200 && recordingActive_.load(std::memory_order_acquire);
       ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

bool AudioEngine::isRecording() const noexcept {
  return recordingActive_.load(std::memory_order_acquire);
}

bool AudioEngine::takeRecordedSamples(std::vector<float>& out,
                                      uint32_t& sampleRate,
                                      uint32_t& channels) noexcept {
  if (recordingActive_.load(std::memory_order_acquire))
    return false; // still live
  const uint64_t n =
      recordingFrames_.load(std::memory_order_acquire);
  if (n == 0)
    return false;
  out.assign(recordBuffer_.begin(),
             recordBuffer_.begin() + static_cast<ptrdiff_t>(n));
  sampleRate = static_cast<uint32_t>(sampleRate_);
  channels = 1;
  return true;
}

bool AudioEngine::renderToWav(const std::string& path, double fromBeats,
                              double toBeats, uint32_t channels,
                              std::string& err) {
  if (channels != 1 && channels != 2) {
    err = "export supports 1 or 2 channels";
    return false;
  }
  if (isRunning()) {
    err = "stop the audio stream before rendering offline";
    return false;
  }
  if (!(fromBeats >= 0.0) || !(toBeats > fromBeats)) {
    err = "invalid render range";
    return false;
  }
  drainControls(); // settle any control backlog we care about

  const double sr = sampleRate_;
  const double fpb = transport_.framesPerBeat();
  const uint64_t f0 = static_cast<uint64_t>(fromBeats * fpb + 0.5);
  const uint64_t f1 = static_cast<uint64_t>(toBeats * fpb + 0.5);
  const uint64_t totalFrames = f1 - f0;

  const PlayState savedState = state_;
  const bool savedLoop = transport_.isLooping();
  transport_.setPosition(f0);
  transport_.play();
  state_ = PlayState::Playing;
  transport_.setLooping(false);
  metronome_.setTicking(false);
  metronome_.clearCountIn();
  countPending_ = false;

  constexpr uint32_t kChunk = 512;
  std::vector<float> scratch(kChunk * channels, 0.f);
  std::vector<float> wav(static_cast<size_t>(totalFrames) * channels, 0.f);
  uint64_t fill = 0;
  for (uint64_t pos = f0; pos < f1; pos += kChunk) {
    const uint32_t n =
        static_cast<uint32_t>(std::min<uint64_t>(kChunk, f1 - pos));
    std::fill(scratch.begin(), scratch.end(), 0.f);
    process(nullptr, 0, scratch.data(), channels, n, pos, sr);
    std::copy_n(scratch.begin(), static_cast<size_t>(n) * channels,
                wav.begin() + static_cast<ptrdiff_t>(fill));
    fill += static_cast<uint64_t>(n) * channels;
  }

  const bool ok = misuc::audio::writeWavSamples(
      path, wav.data(), totalFrames, channels, sr,
      /*pcm16=*/false, err);

  state_ = savedState;
  transport_.pause();
  transport_.setLooping(savedLoop);
  metronome_.setTicking(metronomeEnabled_);
  snapshot_.playing.store(state_ == PlayState::Playing,
                          std::memory_order_relaxed);
  return ok;
}

void AudioEngine::previewNote(double freq, float velocity) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::PreviewNote);
  m.d0 = freq;
  m.f0 = std::clamp(velocity, 0.f, 1.f);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::seekToBeat(double beat) {
  if (!(beat >= 0.0))
    return;
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Seek);
  m.d0 = beat;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::setContentEnd(double endBeats) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::ContentEnd);
  m.d0 = endBeats >= 0.0 ? endBeats : 0.0;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::previewSample(std::vector<float> interleaved,
                                uint32_t channels) {
  if (channels < 1 || channels > 2 || interleaved.empty())
    return;
  // Rate-limit the published buffer size so a stray multi-minute file can't
  // chew through the triple slots; previews are short auditions.
  const size_t cap = static_cast<size_t>(sampleRate_) * 30 * channels;
  if (interleaved.size() > cap)
    interleaved.resize(cap);
  const int c = previewPublishSlot_;
  previewPublishSlot_ = (previewPublishSlot_ + 1) % 3;
  PreviewSlot& slot = previewSlots_[c];
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  slot.samples = std::move(interleaved);
  std::atomic_thread_fence(std::memory_order_release);
  livePreviewSlot_.store(c, std::memory_order_release);
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::PreviewSample);
  m.f0 = static_cast<float>(channels);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

bool AudioEngine::isTransportPlaying() const noexcept {
  return snapshot_.playing.load(std::memory_order_relaxed);
}

uint64_t AudioEngine::playheadSamples() const noexcept {
  return snapshot_.playheadSamples.load(std::memory_order_relaxed);
}

float AudioEngine::meterPeakL() const noexcept {
  return snapshot_.peakL.load(std::memory_order_relaxed);
}
float AudioEngine::meterPeakR() const noexcept {
  return snapshot_.peakR.load(std::memory_order_relaxed);
}
float AudioEngine::meterRmsL() const noexcept {
  return snapshot_.rmsL.load(std::memory_order_relaxed);
}
float AudioEngine::meterRmsR() const noexcept {
  return snapshot_.rmsR.load(std::memory_order_relaxed);
}
bool AudioEngine::meterClipped() const noexcept {
  return snapshot_.clipped.load(std::memory_order_relaxed);
}
float AudioEngine::inMeterPeak() const noexcept {
  return snapshot_.inPeak.load(std::memory_order_relaxed);
}
float AudioEngine::inMeterRms() const noexcept {
  return snapshot_.inRms.load(std::memory_order_relaxed);
}
bool AudioEngine::inMeterClipped() const noexcept {
  return snapshot_.inClipped.load(std::memory_order_relaxed);
}
uint64_t AudioEngine::xruns() const noexcept {
  return snapshot_.xruns.load(std::memory_order_relaxed);
}

float AudioEngine::trackMeterPeakL(uint32_t idx) const noexcept {
  return idx < kMaxTrackCount
             ? trackPeakL_[idx].load(std::memory_order_relaxed)
             : 0.f;
}
float AudioEngine::trackMeterPeakR(uint32_t idx) const noexcept {
  return idx < kMaxTrackCount
             ? trackPeakR_[idx].load(std::memory_order_relaxed)
             : 0.f;
}
float AudioEngine::trackMeterRmsL(uint32_t idx) const noexcept {
  return idx < kMaxTrackCount
             ? trackRmsL_[idx].load(std::memory_order_relaxed)
             : 0.f;
}
float AudioEngine::trackMeterRmsR(uint32_t idx) const noexcept {
  return idx < kMaxTrackCount
             ? trackRmsR_[idx].load(std::memory_order_relaxed)
             : 0.f;
}
bool AudioEngine::trackMeterClipped(uint32_t idx) const noexcept {
  return idx < kMaxTrackCount &&
         trackClipped_[idx].load(std::memory_order_relaxed);
}

void AudioEngine::drainControls() noexcept {
  ControlMessage m;
  while (controlQueue_->pop(m)) {
    switch (static_cast<ControlKind>(m.type)) {
      case ControlKind::Transport: {
        const auto action = static_cast<TransportAction>(static_cast<int>(m.f0));
        switch (action) {
          case TransportAction::Play:
            countPending_ = m.f1 > 0.f;
            countInBeats_ = m.f1;
            beginPlay();
            break;
          case TransportAction::Stop:
            transport_.stop();
            state_ = PlayState::Stopped;
            metronome_.setTicking(false);
            metronome_.clearCountIn();
            midiPlayer_.reset();
            snapshot_.playing.store(false, std::memory_order_relaxed);
            break;
          case TransportAction::Pause:
            transport_.pause();
            state_ = PlayState::Stopped;
            metronome_.setTicking(false);
            metronome_.clearCountIn();
            midiPlayer_.reset();
            snapshot_.playing.store(false, std::memory_order_relaxed);
            break;
        }
        break;
      }
      case ControlKind::SetTempo:
        transport_.setTempo(m.d0);
        metronome_.setTempo(m.d0);
        groove_.setTempo(m.d0);
        midiPlayer_.setTempo(m.d0);
        clipPlayer_.setTempo(m.d0);
        break;
      case ControlKind::Metronome:
        metronomeEnabled_ = m.f0 > 0.f;
        metronome_.setEnabled(metronomeEnabled_);
        metronome_.setVolume(0.9f);
        break;
      case ControlKind::Groove:
        grooveEnabled_ = m.f0 > 0.f;
        groove_.setEnabled(grooveEnabled_);
        break;
      case ControlKind::MonitorGain:
        monitorSmoother_.setTarget(m.f0);
        break;
      case ControlKind::TestTone:
        testTone_ = m.f0 > 0.f;
        if (!testTone_)
          tone_.reset();
        break;
      case ControlKind::MasterGain:
        masterSmoother_.setTarget(m.f0);
        break;
      case ControlKind::PreviewNote:
        midiPlayer_.triggerPreview(static_cast<double>(wallFrames_),
                                   0.35 * sampleRate_, m.d0, m.f0);
        break;
      case ControlKind::Loop: {
        const bool on = m.f0 > 0.f;
        transport_.setLooping(on);
        if (on && m.d1 > m.d0) {
          const double fpb = transport_.framesPerBeat();
          transport_.setLoopRange(
              static_cast<uint64_t>(m.d0 * fpb),
              static_cast<uint64_t>(m.d1 * fpb));
        }
        break;
      }
      case ControlKind::Record: {
        const bool on = m.f0 > 0.f;
        recording_ = on;
        if (on) {
          recordWritePos_ = 0;
          recordingFrames_.store(0, std::memory_order_release);
        } else {
          recordingFrames_.store(recordWritePos_, std::memory_order_release);
        }
        // Release-store after all sample writes so the consumer that sees
        // inactive also sees the final captured bytes.
        recordingActive_.store(on, std::memory_order_release);
        break;
      }
      case ControlKind::Seek: {
        // Jump the playhead to the requested beat; keep the play/pause state
        // (playing continues from the new spot, paused stays paused).
        const double fpb = transport_.framesPerBeat();
        transport_.setPosition(
            static_cast<uint64_t>(std::max(0.0, m.d0) * fpb + 0.5));
        if (!transport_.isPlaying())
          snapshot_.playheadSamples.store(transport_.positionFrames(),
                                          std::memory_order_relaxed);
        break;
      }
      case ControlKind::ContentEnd:
        contentEndBeats_ = m.d0;
        break;
      case ControlKind::PreviewSample: {
        const int ps = livePreviewSlot_.load(std::memory_order_acquire);
        if (ps < 0 || previewSlots_[ps].samples.empty())
          break;
        previewActive_ = true;
        previewChannels_ = m.f0 > 1.f ? 2u : 1u;
        previewStartWall_ = wallFrames_;
        break;
      }
    }
  }
}

void AudioEngine::beginPlay() noexcept {
  if (countPending_ && metronomeEnabled_ && countInBeats_ > 0.f) {
    state_ = PlayState::Counting;
    metronome_.setTicking(true);
    metronome_.beginCountIn(static_cast<uint32_t>(countInBeats_));
    transport_.setPosition(0);
    return;
  }
  // A published empty arrangement has nothing to play: don't start so playback
  // never silently runs a bare timeline. Loop playback (rack pattern live) is
  // unaffected — it publishes a positive content end first.
  if (contentEndBeats_ == 0.0)
    return;
  // Start from the current caret: Stop rewinds to 0, a ruler seek parks the
  // caret wherever the user clicked, and Play resumes from there (DAW
  // behaviour). Counting-in starts clean from bar 1 in the path above.
  state_ = PlayState::Playing;
  transport_.play();
  metronome_.setTicking(metronomeEnabled_);
  midiPlayer_.reset();
  snapshot_.playing.store(true, std::memory_order_relaxed);
}

void AudioEngine::renderTestTone(float* data, uint32_t frames,
                                 uint32_t channels) noexcept {
  // A balanced mono sine at moderate level for the Setup Wizard device test.
  float lvl = 0.10f;
  for (uint32_t i = 0; i < frames; ++i) {
    const float s = tone_.nextSample() * lvl;
    for (uint32_t c = 0; c < channels; ++c)
      data[i * channels + c] += s;
  }
}

void AudioEngine::process(const float* input, uint32_t inputChannels,
                          float* output, uint32_t outputChannels,
                          uint32_t frames, uint64_t framePosition,
                          double sampleRate) noexcept {
  if (output == nullptr || frames == 0 || outputChannels == 0)
    return;

  if (sampleRate_ != sampleRate) {
    sampleRate_ = sampleRate;
    transport_.setSampleRate(sampleRate);
    metronome_.setSampleRate(sampleRate);
    groove_.setSampleRate(sampleRate);
    tone_.setSampleRate(sampleRate);
    midiPlayer_.setSampleRate(sampleRate);
    clipPlayer_.setSampleRate(sampleRate);
    meterL_.setSampleRate(static_cast<float>(sampleRate));
    meterR_.setSampleRate(static_cast<float>(sampleRate));
    inMeter_.setSampleRate(static_cast<float>(sampleRate));
    meterL_.setDecaySeconds(0.35f, 0.30f);
    meterR_.setDecaySeconds(0.35f, 0.30f);
    inMeter_.setDecaySeconds(0.35f, 0.30f);
    for (uint32_t i = 0; i < kMaxTrackCount; ++i) {
      MixerTrackRT& rt = rtTracks_[i];
      rt.meterL.setSampleRate(static_cast<float>(sampleRate));
      rt.meterR.setSampleRate(static_cast<float>(sampleRate));
      rt.meterL.setDecaySeconds(0.35f, 0.30f);
      rt.meterR.setDecaySeconds(0.35f, 0.30f);
    }
  }
  (void)framePosition;

  drainControls();

  std::fill(output, output + static_cast<size_t>(frames) * outputChannels, 0.f);

  // Input monitoring (pre master gain) + capture metering.
  if (input != nullptr && inputChannels > 0) {
    const float g = monitorSmoother_.tick();
    const uint32_t mix = outputChannels > 1 ? 2 : outputChannels;
    for (uint32_t i = 0; i < frames; ++i) {
      const float l = input[i * inputChannels];
      const float r = inputChannels > 1 ? input[i * inputChannels + 1] : l;
      if (mix > 1) {
        output[i * outputChannels] += l * g;
        output[i * outputChannels + 1] += r * g;
      } else if (mix == 1) {
        output[i] += 0.5f * (l + r) * g;
      }
    }
    inMeter_.processInterleaved(const_cast<float*>(input), inputChannels, 0,
                                frames);
  }
  snapshot_.inPeak.store(inMeter_.peak(), std::memory_order_relaxed);
  snapshot_.inRms.store(inMeter_.rms(), std::memory_order_relaxed);
  snapshot_.inClipped.store(inMeter_.clipped(), std::memory_order_relaxed);

  // Audio input recording: capture to a mono ring only while armed and the
  // transport is playing. The buffer is preallocated on the control thread so
  // this path never allocates or blocks.
  if (recording_ && state_ == PlayState::Playing && input != nullptr &&
      inputChannels > 0 && !recordBuffer_.empty()) {
    const uint64_t room =
        kMaxRecordFrames - std::min<uint64_t>(recordWritePos_, kMaxRecordFrames);
    const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(frames, room));
    for (uint32_t i = 0; i < take; ++i) {
      const float l = input[i * inputChannels];
      const float r = inputChannels > 1 ? input[i * inputChannels + 1] : l;
      recordBuffer_[recordWritePos_ + i] = 0.5f * (l + r);
    }
    recordWritePos_ += take;
    recordingFrames_.store(recordWritePos_, std::memory_order_release);
  }

  const uint64_t songPos = transport_.positionFrames(); // block-start position

  // Content-end stop (computed after advance, applied after the per-track mix
  // so the block that straddles the arrangement end still renders its audio).
  bool stopAtEnd = false;
  uint64_t endFrames = 0;

  if (state_ == PlayState::Playing) {
    const uint64_t startPos = songPos;
    transport_.advance(frames);
    if (!transport_.isLooping() && contentEndBeats_ >= 0.0) {
      endFrames = static_cast<uint64_t>(
          contentEndBeats_ * transport_.framesPerBeat() + 0.5);
      if (transport_.positionFrames() >= endFrames)
        stopAtEnd = true;
    }
    metronome_.setTicking(metronomeEnabled_);
    if (outputChannels >= 2) {
      metronome_.render(&output[0], &output[1], frames, outputChannels);
      groove_.render(&output[0], &output[1], frames, outputChannels, startPos);
    } else {
      metronome_.render(&output[0], &output[0], frames, outputChannels);
      groove_.render(&output[0], &output[0], frames, outputChannels, startPos);
    }
    if (countPending_ && metronome_.countInComplete()) {
      metronome_.clearCountIn();
      countPending_ = false;
    }
  } else if (state_ == PlayState::Counting) {
    // Count-in before the transport starts running.
    metronome_.render(outputChannels >= 2 ? &output[0] : &output[0],
                      outputChannels >= 2 ? &output[1] : &output[0], frames,
                      outputChannels);
    if (metronome_.countInComplete()) {
      if (contentEndBeats_ == 0.0) {
        // The count-in finished but the arrangement is empty: stand down.
        state_ = PlayState::Stopped;
        countPending_ = false;
        metronome_.clearCountIn();
        metronome_.setTicking(false);
      } else {
        state_ = PlayState::Playing;
        countPending_ = false;
        metronome_.clearCountIn();
        transport_.play();
        snapshot_.playing.store(true, std::memory_order_relaxed);
      }
    }
  }

// Per-track mix: render each track's MIDI notes and audio clips into its own
  // fixed stereo scratch, apply smoothed fader gain + constant-power pan, meter
  // the post-gain signal, then gate by mute/solo (any-solo logic) and sum onto
  // the output. Runs in every state so note previews still sound while stopped;
  // the score/clip/mixer slots are held only for the block.
  {
    const uint64_t wall0 = wallFrames_;
    wallFrames_ = wall0 + frames; // wall clock advances regardless of play
    const int slot = liveSlot_.load(std::memory_order_acquire);
    const int cslot = liveClipSlot_.load(std::memory_order_acquire);
    const bool includeScore = state_ == PlayState::Playing && slot >= 0;
    const bool includeClips = state_ == PlayState::Playing && cslot >= 0;
    ScoreSlot* const ss = slot >= 0 ? &scoreSlots_[slot] : nullptr;
    ClipSlot* const cs = cslot >= 0 ? &clipSlots_[cslot] : nullptr;
    if (ss)
      ss->refs.fetch_add(1, std::memory_order_relaxed);
    if (cs)
      cs->refs.fetch_add(1, std::memory_order_relaxed);

    // Fold published mixer params into the per-track smoothing/gating targets.
    // The slot holds a small fixed-size array we copy out, so the hold is short.
    const int ms = liveMixerSlot_.load(std::memory_order_acquire);
    bool anySolo = false;
    if (ms >= 0) {
      MixerSlot& mslot = mixerSlots_[ms];
      mslot.refs.fetch_add(1, std::memory_order_relaxed);
      for (uint32_t t = 0; t < kMaxTrackCount; ++t) {
        MixerTrackRT& rt = rtTracks_[t];
        const MixerTrackParam* p =
            t < mslot.params.size() ? &mslot.params[t] : nullptr;
        rt.gain.setTarget(p ? dsp::gainFromDb(p->gainDb) : 1.f);
        const auto [gl, gr] = p ? dsp::panLaw(p->pan) : dsp::panLaw(0.f);
        rt.panL.setTarget(gl);
        rt.panR.setTarget(gr);
        rt.mute = p && p->mute;
        rt.solo = p && p->solo;
        anySolo = anySolo || rt.solo;
      }
      mslot.refs.fetch_sub(1, std::memory_order_release);
    }

    for (uint32_t base = 0; base < frames; base += kMaxBlockFrames) {
      const uint32_t seg = std::min(kMaxBlockFrames, frames - base);
      const uint64_t segPos = songPos + base;
      for (uint32_t t = 0; t < kMaxTrackCount; ++t) {
        float* const sL = &scratchL_[t * kMaxBlockFrames];
        float* const sR = &scratchR_[t * kMaxBlockFrames];
        std::fill(sL, sL + seg, 0.f);
        std::fill(sR, sR + seg, 0.f);
        MixerTrackRT& rt = rtTracks_[t];
        const bool hasScore = ss && includeScore && t < ss->tracks.size() &&
                              !ss->tracks[t].empty();
        const bool hasClips = cs && includeClips && t < cs->tracks.size() &&
                              !cs->tracks[t].empty();
        if (hasScore) {
          if (outputChannels >= 2)
            midiPlayer_.renderScore(ss->tracks[t], true, sL, sR, seg, 2,
                                    segPos);
          else
            midiPlayer_.renderScore(ss->tracks[t], true, sL, sL, seg, 1,
                                    segPos);
        }
        if (hasClips) {
          if (outputChannels >= 2)
            clipPlayer_.render(cs->tracks[t], sL, sR, seg, 2, segPos);
          else
            clipPlayer_.render(cs->tracks[t], sL, sL, seg, 1, segPos);
        }
        // Smoothed fader + pan (constant power) applied in place per sample.
        for (uint32_t n = 0; n < seg; ++n) {
          const float g = rt.gain.tick();
          const float gl = rt.panL.tick();
          const float gr = rt.panR.tick();
          sL[n] *= g * gl;
          sR[n] *= g * gr;
        }
        // Meter the real post-gain signal (independent of mute/solo gating).
        rt.meterL.process(sL, seg);
        rt.meterR.process(sR, seg);
        trackPeakL_[t].store(rt.meterL.peak(), std::memory_order_relaxed);
        trackPeakR_[t].store(rt.meterR.peak(), std::memory_order_relaxed);
        trackRmsL_[t].store(rt.meterL.rms(), std::memory_order_relaxed);
        trackRmsR_[t].store(rt.meterR.rms(), std::memory_order_relaxed);
        trackClipped_[t].store(rt.meterL.clipped() || rt.meterR.clipped(),
                               std::memory_order_relaxed);
        const bool audible = !rt.mute && (!anySolo || rt.solo);
        if (audible) {
          if (outputChannels >= 2) {
            for (uint32_t n = 0; n < seg; ++n) {
              output[(base + n) * outputChannels] += sL[n];
              output[(base + n) * outputChannels + 1] += sR[n];
            }
          } else {
            for (uint32_t n = 0; n < seg; ++n)
              output[base + n] += 0.5f * (sL[n] + sR[n]);
          }
        }
      }
    }
    if (cs)
      cs->refs.fetch_sub(1, std::memory_order_release);
    if (ss)
      ss->refs.fetch_sub(1, std::memory_order_release);

    // Audition previews render once on the wall clock (never per-track, so the
    // mix can't duplicate them).
    if (outputChannels >= 2)
      midiPlayer_.renderPreviews(&output[0], &output[1], frames, outputChannels,
                                 wall0);
    else
      midiPlayer_.renderPreviews(&output[0], &output[0], frames, outputChannels,
                                 wall0);

    // One-shot sample audition (browser preview). Renders against the wall
    // clock with a short attack/release so a preview never clicks; stops once
    // the published buffer has been consumed.
    if (previewActive_) {
      const int ps = livePreviewSlot_.load(std::memory_order_acquire);
      PreviewSlot* pv =
          (ps >= 0 && !previewSlots_[ps].samples.empty()) ? &previewSlots_[ps]
                                                          : nullptr;
      if (!pv) {
        previewActive_ = false;
      } else {
        pv->refs.fetch_add(1, std::memory_order_relaxed);
        const uint32_t ch = previewChannels_;
        const uint32_t outCh = outputChannels > 1 ? 2 : 1;
        const uint64_t totalF =
            static_cast<uint64_t>(pv->samples.size()) / ch;
        const uint32_t edge =
            static_cast<uint32_t>(std::min<uint64_t>(256, totalF / 2));
        for (uint32_t i = 0; i < frames; ++i) {
          const uint64_t idx = (wall0 + i) - previewStartWall_;
          if (idx >= totalF) {
            previewActive_ = false;
            break;
          }
          const size_t s0 = static_cast<size_t>(idx) * ch;
          float l = pv->samples[s0];
          float r = ch == 2 ? pv->samples[s0 + 1] : l;
          if (outCh == 1)
            l = r = 0.5f * (l + r);
          float env = 1.f;
          if (idx < edge)
            env = static_cast<float>(idx + 1) / edge;
          else if (idx >= totalF - edge)
            env = static_cast<float>(totalF - idx) / edge;
          if (outputChannels >= 2) {
            output[i * outputChannels] += l * env;
            output[i * outputChannels + 1] += r * env;
          } else {
            output[i] += 0.5f * (l + r) * env;
          }
        }
        pv->refs.fetch_sub(1, std::memory_order_release);
      }
    }
  }

  // Park the caret exactly at the content end and stop normal playback. The
  // audio for the straddling block has already been rendered above.
  if (stopAtEnd) {
    transport_.setPosition(endFrames);
    transport_.pause();
    state_ = PlayState::Stopped;
    metronome_.setTicking(false);
    metronome_.clearCountIn();
    midiPlayer_.reset();
    snapshot_.playing.store(false, std::memory_order_relaxed);
  }

  if (testTone_)
    renderTestTone(output, frames, outputChannels);

  // Master bus gain (smoothed to avoid clicks) and metering.
  for (uint32_t i = 0; i < frames; ++i) {
    const float g = masterSmoother_.tick();
    for (uint32_t c = 0; c < outputChannels; ++c)
      output[i * outputChannels + c] *= g;
  }

  if (outputChannels >= 2) {
    meterL_.processInterleaved(output, outputChannels, 0, frames);
    meterR_.processInterleaved(output, outputChannels, 1, frames);
  }
  snapshot_.peakL.store(meterL_.peak(), std::memory_order_relaxed);
  snapshot_.peakR.store(meterR_.peak(), std::memory_order_relaxed);
  snapshot_.rmsL.store(meterL_.rms(), std::memory_order_relaxed);
  snapshot_.rmsR.store(meterR_.rms(), std::memory_order_relaxed);
  snapshot_.clipped.store(meterL_.clipped() || meterR_.clipped(),
                          std::memory_order_relaxed);
  snapshot_.playheadSamples.store(transport_.positionFrames(),
                                  std::memory_order_relaxed);
  snapshot_.playing.store(state_ == PlayState::Playing,
                          std::memory_order_relaxed);
  snapshot_.xruns.store(backendStatus().xruns, std::memory_order_relaxed);
}

} // namespace misuc::engine