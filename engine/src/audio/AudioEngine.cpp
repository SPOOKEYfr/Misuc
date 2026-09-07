#include <misuc/engine/audio/AudioEngine.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include <misuc/engine/audio/PipeWireBackend.h>
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
  lastProcessPosition_ = 0;
  snapshot_.clear();

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
  slot.notes = std::move(notes);
  std::atomic_thread_fence(std::memory_order_release);
  liveSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearScore() {
  setScore(std::vector<dsp::MidiNote>());
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
  state_ = PlayState::Playing;
  transport_.setPosition(0);
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
    meterL_.setSampleRate(static_cast<float>(sampleRate));
    meterR_.setSampleRate(static_cast<float>(sampleRate));
    inMeter_.setSampleRate(static_cast<float>(sampleRate));
    meterL_.setDecaySeconds(0.35f, 0.30f);
    meterR_.setDecaySeconds(0.35f, 0.30f);
    inMeter_.setDecaySeconds(0.35f, 0.30f);
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

  if (state_ == PlayState::Playing) {
    const uint64_t startPos = transport_.positionFrames();
    transport_.advance(frames);
    metronome_.setTicking(metronomeEnabled_);
    if (outputChannels >= 2) {
      metronome_.render(&output[0], &output[1], frames, outputChannels);
      groove_.render(&output[0], &output[1], frames, outputChannels, startPos);
    } else {
      metronome_.render(&output[0], &output[0], frames, outputChannels);
      groove_.render(&output[0], &output[0], frames, outputChannels, startPos);
    }
    const int slot = liveSlot_.load(std::memory_order_acquire);
    if (slot >= 0) {
      ScoreSlot& scoreSlot = scoreSlots_[slot];
      scoreSlot.refs.fetch_add(1, std::memory_order_relaxed);
      if (outputChannels >= 2) {
        midiPlayer_.render(scoreSlot.notes, &output[0], &output[1], frames,
                           outputChannels, startPos);
      } else {
        midiPlayer_.render(scoreSlot.notes, &output[0], &output[0], frames,
                           outputChannels, startPos);
      }
      scoreSlot.refs.fetch_sub(1, std::memory_order_release);
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
      state_ = PlayState::Playing;
      countPending_ = false;
      metronome_.clearCountIn();
      transport_.play();
      snapshot_.playing.store(true, std::memory_order_relaxed);
    }
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