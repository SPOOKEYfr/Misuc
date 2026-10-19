#include <misuc/engine/audio/AudioEngine.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

// The default backend is platform-selected: PipeWire on Linux, WASAPI shared
// mode on Windows (both satisfy the same AudioBackend contract).
#if defined(_WIN32)
#include <misuc/engine/audio/WASAPIBackend.h>
#else
#include <misuc/engine/audio/PipeWireBackend.h>
#endif
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/Logger.h>
#include <misuc/engine/dsp/Dsp.h>

using misuc::core::logInfo;

namespace misuc::engine {

namespace {
#if defined(_WIN32)
std::unique_ptr<AudioBackend> defaultBackend() {
  return std::make_unique<WASAPIBackend>();
}
#else
std::unique_ptr<AudioBackend> defaultBackend() {
  return std::make_unique<PipeWireBackend>();
}
#endif
} // namespace

AudioEngine::~AudioEngine() {
  // Tear down the backend before any member buffer is freed: the real-time
  // process loop runs on its own thread and may be mid-process() touching
  // meter/scratch allocations declared above. closeAudio() stops the backend
  // (which joins that thread), so call it before members are destroyed
  // normally by ~AudioEngine()'s implicit member teardown.
  if (backend_ && backend_->isRunning())
    closeAudio();
}

AudioEngine::AudioEngine(std::unique_ptr<AudioBackend> backend)
    : backend_(std::move(backend ? std::move(backend) : defaultBackend())),
      controlQueue_(std::make_unique<ControlQueue>()) {
  // ~5 ms fade coefficient at 48 kHz.
  masterSmoother_.setCoefficient(0.004f);
  monitorSmoother_.setCoefficient(0.004f);
  meterL_.setDecaySeconds(0.35f, 0.30f);
  meterR_.setDecaySeconds(0.35f, 0.30f);
  inMeter_.setDecaySeconds(0.35f, 0.30f);
  setMasterGainDb(0.f);
  setMonitorGainDb(0.f);
  scratchL_.assign(kMaxTrackCount * kMaxBlockFrames, 0.f);
  scratchR_.assign(kMaxTrackCount * kMaxBlockFrames, 0.f);
  busScratchL_.assign(kMaxBusCount * kMaxBlockFrames, 0.f);
  busScratchR_.assign(kMaxBusCount * kMaxBlockFrames, 0.f);
  clipPlayer_.setSampleRate(sampleRate_);
  clipPlayer_.setTempo(transport_.tempo());
  // Offline render (renderToFile) can run without start(), so the metronome
  // must be fully configured at construction: its decay depends on the sample
  // rate and a zero decay would silence every click.
  metronome_.setSampleRate(sampleRate_);
  metronome_.setTempo(transport_.tempo());
  metronome_.setTimeSignature(transport_.timeSigNumerator(),
                              transport_.timeSigDenominator());
  metronome_.reset();
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
  for (uint32_t i = 0; i < kMaxBusCount; ++i) {
    BusRT& brt = rtBuses_[i];
    brt.gain.setCoefficient(0.004f);
    brt.panL.setCoefficient(0.004f);
    brt.panR.setCoefficient(0.004f);
    brt.gain.reset(1.f);
    brt.panL.reset(0.707106781f); // constant-power centre
    brt.panR.reset(0.707106781f);
    brt.meterL.setSampleRate(static_cast<float>(sampleRate_));
    brt.meterR.setSampleRate(static_cast<float>(sampleRate_));
    brt.meterL.setDecaySeconds(0.35f, 0.30f);
    brt.meterR.setDecaySeconds(0.35f, 0.30f);
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
    rt.busIndex = -1;
    trackPeakL_[i].store(0.f, std::memory_order_relaxed);
    trackPeakR_[i].store(0.f, std::memory_order_relaxed);
    trackRmsL_[i].store(0.f, std::memory_order_relaxed);
    trackRmsR_[i].store(0.f, std::memory_order_relaxed);
    trackClipped_[i].store(false, std::memory_order_relaxed);
  }
  for (uint32_t i = 0; i < kMaxBusCount; ++i) {
    BusRT& brt = rtBuses_[i];
    brt.meterL.clearClip();
    brt.meterR.clearClip();
    brt.gain.reset(1.f);
    brt.panL.reset(0.707106781f);
    brt.panR.reset(0.707106781f);
    brt.mute = false;
    brt.solo = false;
    busPeakL_[i].store(0.f, std::memory_order_relaxed);
    busPeakR_[i].store(0.f, std::memory_order_relaxed);
    busRmsL_[i].store(0.f, std::memory_order_relaxed);
    busRmsR_[i].store(0.f, std::memory_order_relaxed);
    busClipped_[i].store(false, std::memory_order_relaxed);
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

void AudioEngine::setMetronome(bool enabled, float countInBeats,
                              float volume) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::Metronome);
  m.f0 = enabled ? 1.f : 0.f;
  m.f1 = countInBeats;
  m.f2 = volume;
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

void AudioEngine::setMasterLimiter(bool on) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::MasterLimiter);
  m.f0 = on ? 1.f : 0.f;
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
  publishMixer(std::move(params), std::vector<MixerBusParam>(),
               std::vector<MixerSendParam>());
}

void AudioEngine::publishMixer(std::vector<MixerTrackParam> params,
                               std::vector<MixerBusParam> buses) {
  publishMixer(std::move(params), std::move(buses),
               std::vector<MixerSendParam>());
}

void AudioEngine::publishMixer(std::vector<MixerTrackParam> params,
                               std::vector<MixerBusParam> buses,
                               std::vector<MixerSendParam> sends) {
  if (params.size() > kMaxTrackCount)
    params.resize(kMaxTrackCount);
  if (buses.size() > kMaxBusCount)
    buses.resize(kMaxBusCount);
  if (sends.size() > kMaxSends)
    sends.resize(kMaxSends);
  const int c = mixerPublishSlot_;
  mixerPublishSlot_ = (mixerPublishSlot_ + 1) % 3;
  MixerSlot& slot = mixerSlots_[c];
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  slot.params = std::move(params);

  const int bc = busPublishSlot_;
  busPublishSlot_ = (busPublishSlot_ + 1) % 3;
  BusSlot& bslot = busSlots_[bc];
  while (bslot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  bslot.params = std::move(buses);
  std::atomic_thread_fence(std::memory_order_release);
  liveBusSlot_.store(bc, std::memory_order_release);

  // Sends are published in the same snapshot so a route change cannot leave a
  // send aimed at a stale bus index.
  const int sc = sendPublishSlot_;
  sendPublishSlot_ = (sendPublishSlot_ + 1) % 3;
  SendSlot& sslot = sendSlots_[sc];
  while (sslot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  sslot.params = std::move(sends);
  std::atomic_thread_fence(std::memory_order_release);
  liveSendSlot_.store(sc, std::memory_order_release);

  std::atomic_thread_fence(std::memory_order_release);
  liveMixerSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearMixer() {
  publishMixer(std::vector<MixerTrackParam>(), std::vector<MixerBusParam>(),
               std::vector<MixerSendParam>());
}

void AudioEngine::publishAutomation(std::vector<AutomationParam> params) {
  const int c = automationPublishSlot_;
  automationPublishSlot_ = (automationPublishSlot_ + 1) % 3;
  AutomationSlot& slot = automationSlots_[c];
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  slot.params = std::move(params);
  std::atomic_thread_fence(std::memory_order_release);
  liveAutomationSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearAutomation() {
  publishAutomation(std::vector<AutomationParam>());
}

Lv2Host* AudioEngine::fxHost() {
  if (!fxHost_)
    fxHost_ = std::make_unique<Lv2Host>();
  return fxHost_.get();
}

bool AudioEngine::fxSupported() noexcept {
  return fxHost()->available();
}

std::vector<FxPluginInfo> AudioEngine::discoverFx() {
  std::vector<FxPluginInfo> out = fxHost()->discoverEffects();
  out.push_back(builtinEqInfo());
  out.push_back(builtinCompInfo());
  return out;
}

const FxPluginInfo* AudioEngine::fxInfo(const std::string& uri) const {
  if (uri == kBuiltinEqUri)
    return &builtinEqInfo();
  if (uri == kBuiltinCompUri)
    return &builtinCompInfo();
  return fxHost_ ? fxHost_->find(uri) : nullptr;
}

void AudioEngine::publishFx(std::vector<std::vector<FxInsertParam>> chains) {
  publishFx(std::move(chains), std::vector<std::vector<FxInsertParam>>());
}

void AudioEngine::publishFx(std::vector<std::vector<FxInsertParam>> chains,
                            std::vector<std::vector<FxInsertParam>> busChains) {
  const float sr = sampleRate_;
  const int c = fxPublishSlot_;
  fxPublishSlot_ = (fxPublishSlot_ + 1) % 3;
  FxSlot& slot = fxSlots_[c];
  while (slot.refs.load(std::memory_order_acquire) != 0)
    std::this_thread::yield();
  // Tear down the previously published instances in this slot on the control
  // thread now that the audio thread has left it (never on the RT thread).
  const auto teardown = [&](std::vector<std::vector<FxPluginRT>>& list) {
    for (auto& chain : list)
      for (FxPluginRT& p : chain)
        if (p.handle) {
          if (p.kind == FxPluginRT::Kind::BuiltinEq)
            delete static_cast<dsp::ParametricEq*>(p.handle);
          else if (p.kind == FxPluginRT::Kind::BuiltinComp)
            delete static_cast<dsp::Compressor*>(p.handle);
          else if (fxHost_)
            fxHost_->freeInstance(p.handle);
        }
  };
  teardown(slot.chains);
  teardown(slot.busChains);
  slot.chains.clear();
  slot.busChains.clear();
  if (chains.size() > kMaxTrackCount)
    chains.resize(kMaxTrackCount);
  slot.chains.resize(chains.size());
  if (busChains.size() > kMaxBusCount)
    busChains.resize(kMaxBusCount);
  slot.busChains.resize(busChains.size());

  Lv2Host* host = fxHost(); // lazily creates the lilv world when published
  const bool lv2 = host && host->available();
  // Build the dense EQ parameter vector (kPortCount floats) from the insert's
  // sparse FxParam list, filling unspecified ports with the make-neutral defaults.
  const auto eqPorts = [](const std::vector<FxParam>& params) {
    std::vector<float> ports(dsp::ParametricEq::kPortCount);
    dsp::ParametricEq::defaultPorts(ports.data(),
                                    static_cast<uint32_t>(ports.size()));
    for (const FxParam& p : params)
      if (p.index < ports.size())
        ports[p.index] = p.value;
    return ports;
  };
  const auto compPorts = [](const std::vector<FxParam>& params) {
    std::vector<float> ports(dsp::Compressor::kPortCount);
    dsp::Compressor::defaultPorts(ports.data(),
                                  static_cast<uint32_t>(ports.size()));
    for (const FxParam& p : params)
      if (p.index < ports.size())
        ports[p.index] = p.value;
    return ports;
  };
  // Instantiate one insert; returns an instance or nothing (unknown / unhosed).
  const auto instantiate = [&](const FxInsertParam& ins, int scSource) {
    if (ins.uri.empty())
      return FxPluginRT{};
    if (ins.uri == kBuiltinEqUri) {
      std::vector<float> ports = eqPorts(ins.params);
      auto* eq = new dsp::ParametricEq(sr, ports.data(),
                                       static_cast<uint32_t>(ports.size()));
      FxPluginRT p;
      p.handle = eq;
      p.kind = FxPluginRT::Kind::BuiltinEq;
      p.enabled = ins.enabled;
      p.scSource = scSource;
      return p;
    }
    if (ins.uri == kBuiltinCompUri) {
      std::vector<float> ports = compPorts(ins.params);
      auto* comp = new dsp::Compressor(sr, ports.data(),
                                       static_cast<uint32_t>(ports.size()));
      FxPluginRT p;
      p.handle = comp;
      p.kind = FxPluginRT::Kind::BuiltinComp;
      p.enabled = ins.enabled;
      p.scSource = scSource;
      return p;
    }
    if (lv2) {
      if (void* h = host->instantiate(ins.uri, ins.params, sr,
                                      kMaxBlockFrames)) {
        FxPluginRT p;
        p.handle = h;
        p.kind = FxPluginRT::Kind::Lv2;
        p.enabled = ins.enabled;
        p.scSource = scSource;
        return p;
      }
    }
    return FxPluginRT{};
  };
  for (size_t t = 0; t < chains.size(); ++t) {
    for (const FxInsertParam& ins : chains[t]) {
      FxPluginRT p = instantiate(ins, -1); // track chains never host sidechain
      if (p.handle)
        slot.chains[t].push_back(std::move(p));
    }
  }
  for (size_t b = 0; b < busChains.size(); ++b) {
    for (const FxInsertParam& ins : busChains[b]) {
      // The sidechain key is another bus's post-fader output.
      const int sc = ins.sidechainBus >= 0 &&
                             ins.sidechainBus < static_cast<int>(kMaxBusCount)
                         ? ins.sidechainBus
                         : -1;
      FxPluginRT p = instantiate(ins, sc);
      if (p.handle)
        slot.busChains[b].push_back(std::move(p));
    }
  }
  std::atomic_thread_fence(std::memory_order_release);
  liveFxSlot_.store(c, std::memory_order_release);
}

void AudioEngine::clearFx() {
  publishFx(std::vector<std::vector<FxInsertParam>>(),
            std::vector<std::vector<FxInsertParam>>());
}

void AudioEngine::startRecord() {
  // Allocate outside the real-time thread; the audio thread only writes.
  recordBuffer_.assign(static_cast<size_t>(kMaxRecordFrames), 0.f);
  recordWritePos_ = 0;
  recordingFrames_.store(0, std::memory_order_relaxed);
  // Phase Lat: read the reported input latency on the control side and push it
  // through the queue BEFORE the record control, so the audio thread applies
  // the compensation to the capture latch before any sample lands. The offset
  // is clamped (never negative) and 0 when the backend reports none (headless
  // runs, tests without a live stream).
  const double inputMs =
      inputLatencyMsOverride_ >= 0.0
          ? inputLatencyMsOverride_
          : (backend_ ? backend_->latency().inputMs : 0.0);
  const int64_t offset = std::max<int64_t>(
      0, static_cast<int64_t>(std::llround(inputMs * sampleRate_ / 1000.0)));
  {
    ControlMessage m;
    m.type = static_cast<uint32_t>(ControlKind::SetCaptureOffset);
    m.d0 = static_cast<double>(offset);
    if (!controlQueue_->push(m))
      controlsDropped_.fetch_add(1);
  }
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

bool AudioEngine::takeRecord(RecordCapture& out) noexcept {
  if (recordingActive_.load(std::memory_order_acquire))
    return false; // still live
  const uint64_t n =
      recordingFrames_.load(std::memory_order_acquire);
  if (n == 0)
    return false;
  out.samples.assign(recordBuffer_.begin(),
                     recordBuffer_.begin() + static_cast<ptrdiff_t>(n));
  out.sampleRate = static_cast<uint32_t>(sampleRate_);
  out.channels = 1;
  const double fpb = transport_.framesPerBeat();
  out.startBeats = fpb > 0.0 ? static_cast<double>(recordStartFrame_) / fpb
                             : 0.0;
  out.endBeats = fpb > 0.0 ? static_cast<double>(recordLastWriteEnd_) / fpb
                           : static_cast<double>(n) / (sampleRate_ > 0
                                                           ? sampleRate_
                                                           : 48000.0);
  out.framesLostTail = recordFramesLost_;
  out.captureOffsetFrames = captureOffsetFrames_;
  return true;
}

int64_t AudioEngine::captureOffsetFrames() const noexcept {
  return captureOffsetApplied_.load(std::memory_order_acquire);
}

bool AudioEngine::renderToFile(const std::string& path, double fromBeats,
                               double toBeats, uint32_t channels,
                               misuc::audio::ExportFormat format, bool dither,
                               std::string& err, uint32_t exportSampleRate,
                               const std::string& title,
                               const std::string& artist,
                               bool includeMetronome,
                               const std::function<bool(double)>& progress) {
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
  // The offline flag silences the metronome by default; the exporter may
  // explicitly ask to include it in the bounce. "Include" is independent of
  // the live transport toggle, so force the click on for the render and
  // restore the user's setting afterwards.
  const bool savedMetEnabled = metronomeEnabled_;
  if (includeMetronome)
    metronome_.setEnabled(true);
  metronome_.setTicking(includeMetronome);
  renderIncludeMetronome_.store(includeMetronome, std::memory_order_relaxed);
  metronome_.clearCountIn();
  countPending_ = false;
  offlineRender_.store(true, std::memory_order_relaxed);

  constexpr uint32_t kChunk = 512;
  std::vector<float> scratch(kChunk * channels, 0.f);
  std::vector<float> wav(static_cast<size_t>(totalFrames) * channels, 0.f);
  uint64_t fill = 0;
  for (uint64_t pos = f0; pos < f1; pos += kChunk) {
    if (progress) {
      const double frac = static_cast<double>(pos - f0) /
                          static_cast<double>(f1 - f0);
      if (!progress(std::clamp(frac, 0.0, 1.0))) {
        err = "export cancelled";
        state_ = savedState;
        transport_.pause();
        transport_.setLooping(savedLoop);
        metronome_.setEnabled(savedMetEnabled);
        metronome_.setTicking(metronomeEnabled_);
        renderIncludeMetronome_.store(false, std::memory_order_relaxed);
        offlineRender_.store(false, std::memory_order_relaxed);
        snapshot_.playing.store(state_ == PlayState::Playing,
                                std::memory_order_relaxed);
        return false;
      }
    }
    const uint32_t n =
        static_cast<uint32_t>(std::min<uint64_t>(kChunk, f1 - pos));
    std::fill(scratch.begin(), scratch.end(), 0.f);
    process(nullptr, 0, scratch.data(), channels, n, pos, sr);
    std::copy_n(scratch.begin(), static_cast<size_t>(n) * channels,
                wav.begin() + static_cast<ptrdiff_t>(fill));
    fill += static_cast<uint64_t>(n) * channels;
  }

  // Phase R: the project may carry a per-project export rate different from
  // the engine's native mixing rate. Render at the native rate (above), then
  // resample the frame payload so the file's absolute duration and pitch are
  // preserved at the requested frame rate. No-op when unset or already equal.
  const uint32_t outSr = exportSampleRate != 0 ? exportSampleRate
                                               : static_cast<uint32_t>(
                                                     std::llround(sr));
  const bool resampleNeeded =
      outSr != static_cast<uint32_t>(std::llround(sr));
  std::vector<float> outAudio;
  uint64_t outFrames = totalFrames;
  const float* payload = wav.data();
  if (resampleNeeded) {
    outFrames = static_cast<uint64_t>(std::llround(
        static_cast<double>(totalFrames) * outSr / sr));
    outFrames = std::max<uint64_t>(outFrames, 1);
    outAudio.resize(static_cast<size_t>(outFrames) * channels);
    misuc::dsp::resampleLinear(wav.data(), totalFrames, channels, outFrames,
                               outAudio.data());
    payload = outAudio.data();
  }

  const bool ok = misuc::audio::writeAudioFile(
      path, payload, outFrames, channels, outSr, format, dither, err, title,
      artist);

  state_ = savedState;
  transport_.pause();
  transport_.setLooping(savedLoop);
  metronome_.setEnabled(savedMetEnabled);
  metronome_.setTicking(metronomeEnabled_);
  renderIncludeMetronome_.store(false, std::memory_order_relaxed);
  offlineRender_.store(false, std::memory_order_relaxed);
  snapshot_.playing.store(state_ == PlayState::Playing,
                          std::memory_order_relaxed);
  return ok;
}

bool AudioEngine::renderToWav(const std::string& path, double fromBeats,
                              double toBeats, uint32_t channels,
                              std::string& err) {
  return renderToFile(path, fromBeats, toBeats, channels,
                      misuc::audio::ExportFormat::WavFloat, /*dither=*/false,
                      err);
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
                                uint32_t channels, float gain) {
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
  m.f1 = gain;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::previewPause(bool paused) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::PreviewPause);
  m.f0 = paused ? 1.f : 0.f;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::previewStop() {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::PreviewStop);
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

void AudioEngine::previewGain(float gain) {
  ControlMessage m;
  m.type = static_cast<uint32_t>(ControlKind::PreviewGain);
  m.f0 = gain;
  if (!controlQueue_->push(m))
    controlsDropped_.fetch_add(1);
}

bool AudioEngine::previewActive() const noexcept {
  return previewActiveSnapshot_.load(std::memory_order_relaxed);
}

double AudioEngine::previewPositionFrames() const noexcept {
  return static_cast<double>(
      previewPosSnapshot_.load(std::memory_order_relaxed));
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

float AudioEngine::busMeterPeakL(uint32_t idx) const noexcept {
  return idx < kMaxBusCount ? busPeakL_[idx].load(std::memory_order_relaxed)
                            : 0.f;
}
float AudioEngine::busMeterPeakR(uint32_t idx) const noexcept {
  return idx < kMaxBusCount ? busPeakR_[idx].load(std::memory_order_relaxed)
                            : 0.f;
}
float AudioEngine::busMeterRmsL(uint32_t idx) const noexcept {
  return idx < kMaxBusCount ? busRmsL_[idx].load(std::memory_order_relaxed)
                            : 0.f;
}
float AudioEngine::busMeterRmsR(uint32_t idx) const noexcept {
  return idx < kMaxBusCount ? busRmsR_[idx].load(std::memory_order_relaxed)
                            : 0.f;
}
bool AudioEngine::busMeterClipped(uint32_t idx) const noexcept {
  return idx < kMaxBusCount &&
         busClipped_[idx].load(std::memory_order_relaxed);
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
        metronomeVolume_ =
            std::clamp(m.f2, 0.f, 1.f);
        metronome_.setVolume(metronomeVolume_);
        // The metronome owns its count-in length: the number of click beats
        // configured by the UI (setMetronome), not the play() request flag.
        countInBeats_ = m.f1;
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
      case ControlKind::MasterLimiter:
        limiterOn_ = m.f0 > 0.f;
        if (!limiterOn_)
          limiter_.reset();
        break;
      case ControlKind::SetCaptureOffset: {
        const int64_t off =
            std::max<int64_t>(0, static_cast<int64_t>(std::llround(m.d0)));
        captureOffsetFrames_ = off;
        captureOffsetApplied_.store(off, std::memory_order_release);
        break;
      }
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
          recordStartLatched_ = false;
          recordLastWriteEnd_ = 0;
          recordFramesLost_ = 0;
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
        previewPaused_ = false;
        previewPosFrames_ = 0;
        previewStartWall_ = wallFrames_;
        if (m.f1 > 0.0f)
          previewGain_ = m.f1;
        break;
      }
      case ControlKind::PreviewGain:
        previewGain_ = std::clamp(m.f0, 0.f, 4.f);
        break;
      case ControlKind::PreviewPause:
        previewPaused_ = m.f0 > 0.0f;
        break;
      case ControlKind::PreviewStop:
        previewActive_ = false;
        previewPaused_ = false;
        previewPosFrames_ = 0;
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

  // Input monitoring (pre master gain) + capture metering. Smoothed per sample
  // so a target change ramps within a block rather than sitting at its old
  // value (one tick per block left live input near-silent for many seconds).
  if (input != nullptr && inputChannels > 0) {
    const uint32_t mix = outputChannels > 1 ? 2 : outputChannels;
    if (inputMonitor_.load(std::memory_order_relaxed)) {
      for (uint32_t i = 0; i < frames; ++i) {
        const float g = monitorSmoother_.tick();
        const float l = input[i * inputChannels];
        const float r = inputChannels > 1 ? input[i * inputChannels + 1] : l;
        if (mix > 1) {
          output[i * outputChannels] += l * g;
          output[i * outputChannels + 1] += r * g;
        } else if (mix == 1) {
          output[i] += 0.5f * (l + r) * g;
        }
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
    const uint64_t blockStart = transport_.positionFrames();
    // Phase Lat: the input samples that arrive in this block were performed
    // captureOffsetFrames_ earlier (device + buffer delay reported by the
    // backend). Shift the take's span back so it lands where played; clamped
    // at song start so a take never begins before beat 0.
    const int64_t off = captureOffsetFrames_;
    auto shift = [off](uint64_t f) -> uint64_t {
      return off > 0 ? (f >= static_cast<uint64_t>(off)
                            ? f - static_cast<uint64_t>(off)
                            : 0)
                     : f;
    };
    if (!recordStartLatched_) {
      recordStartFrame_ = shift(blockStart);
      recordStartLatched_ = true;
    }
    const uint64_t room =
        kMaxRecordFrames - std::min<uint64_t>(recordWritePos_, kMaxRecordFrames);
    const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(frames, room));
    for (uint32_t i = 0; i < take; ++i) {
      const float l = input[i * inputChannels];
      const float r = inputChannels > 1 ? input[i * inputChannels + 1] : l;
      recordBuffer_[recordWritePos_ + i] = 0.5f * (l + r);
    }
    recordWritePos_ += take;
    if (take < frames)
      recordFramesLost_ += frames - take;
    recordLastWriteEnd_ = shift(blockStart + take);
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
    metronome_.setTicking(
        offlineRender_.load(std::memory_order_relaxed)
            ? renderIncludeMetronome_.load(std::memory_order_relaxed)
            : metronomeEnabled_);
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
  // the score/clip/mixer slots are held only for the block. When nothing is
  // scheduled the whole block is parked (idleSilent) to avoid synthesising
  // silence at ~50% core on the audio thread.
  {
    const uint64_t wall0 = wallFrames_;
    wallFrames_ = wall0 + frames; // wall clock advances regardless of play
    const int slot = liveSlot_.load(std::memory_order_acquire);
    const int cslot = liveClipSlot_.load(std::memory_order_acquire);
    // Idle park: stopped transport + no score/clip live slot + no input
    // recording + no browser sample audition + no midi note preview audible in
    // this block's wall window -> the per-track mix, bus/send/fx routing and
    // per-track meters below would only process silence. Skip them and stay
    // idle; the master gain/limiter tail below still runs so the meters and
    // snapshots stay honest.
    const bool idleSilent =
        state_ == PlayState::Stopped && !recording_ && !previewActive_ &&
        slot < 0 && cslot < 0 &&
        !midiPlayer_.hasAudiblePreviews(wall0, wall0 + frames);
    if (!idleSilent) {
    const bool includeScore = state_ == PlayState::Playing && slot >= 0;
    const bool includeClips = state_ == PlayState::Playing && cslot >= 0;
    ScoreSlot* const ss = slot >= 0 ? &scoreSlots_[slot] : nullptr;
    ClipSlot* const cs = cslot >= 0 ? &clipSlots_[cslot] : nullptr;
    if (ss)
      ss->refs.fetch_add(1, std::memory_order_relaxed);
    if (cs)
      cs->refs.fetch_add(1, std::memory_order_relaxed);

    // Fold published mixer params into the per-track smoothing/gating targets.
    // The slots hold small fixed-size arrays we copy out, so the holds are
    // short. Tracks route through a bus (fader/pan/mute/solo applied after the
    // track stage) or straight to the master when busIndex < 0.
    const int ms = liveMixerSlot_.load(std::memory_order_acquire);
    const int bs = liveBusSlot_.load(std::memory_order_acquire);
    const int ss2 = liveSendSlot_.load(std::memory_order_acquire);
    const int fs = liveFxSlot_.load(std::memory_order_acquire);
    SendSlot* sendSlot = nullptr;
    if (ss2 >= 0) {
      sendSlot = &sendSlots_[ss2];
      sendSlot->refs.fetch_add(1, std::memory_order_relaxed);
      for (size_t i = 0; i < sendSlot->params.size() && i < kMaxSends; ++i)
        rtSends_[i].level.setTarget(sendSlot->params[i].level);
    }
     // Hold the fx slot up front: the bus render-order compile reads its bus
    // chains (sidechain deps), so the hold must precede that read. The same
    // slot is then reused to render track and bus chains for the whole block.
    FxSlot* fxSlot = nullptr;
    if (fs >= 0 &&
        (!fxSlots_[fs].chains.empty() || !fxSlots_[fs].busChains.empty())) {
      fxSlot = &fxSlots_[fs];
      fxSlot->refs.fetch_add(1, std::memory_order_relaxed);
    }
    bool anyTrackSolo = false;
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
        rt.busIndex = p && p->busIndex >= 0 ? p->busIndex : -1;
        if (rt.solo)
          anyTrackSolo = true;
      }
      mslot.refs.fetch_sub(1, std::memory_order_release);
    }
    bool anyBusSolo = false;
    if (bs >= 0) {
      BusSlot& bslot = busSlots_[bs];
      bslot.refs.fetch_add(1, std::memory_order_relaxed);
      for (uint32_t b = 0; b < kMaxBusCount; ++b) {
        BusRT& brt = rtBuses_[b];
        const MixerBusParam* p =
            b < bslot.params.size() ? &bslot.params[b] : nullptr;
        brt.gain.setTarget(p ? dsp::gainFromDb(p->gainDb) : 1.f);
        const auto [gl, gr] = p ? dsp::panLaw(p->pan) : dsp::panLaw(0.f);
        brt.panL.setTarget(gl);
        brt.panR.setTarget(gr);
        brt.mute = p && p->mute;
        brt.solo = p && p->solo;
        brt.target = p ? p->target : -1;
        if (brt.solo)
          anyBusSolo = true;
      }

      // Compile the bus render order for this block. Two passes over fixed
      // arrays only (this thread never allocates):
      //   1. Route-cycle rescue: buses cascade through `target`, so a cycle
      //      (a bus feeding itself transitively) would recirculate block to
      //      block. Walk the route edges and break any back edge to the master.
      //   2. Topological sort over dependency edges: a bus renders after every
      //      bus it depends on — its own route children (which flow into it),
      //      the key buses its inserts key on, and the sources of sends aimed
      //      at it. A dependency that would cycle (a bus keying on itself via a
      //      send) is dropped.
      enum State : uint8_t { kUnseen = 0, kVisiting = 1, kDone = 2 };
      {
        State state[kMaxBusCount] = {};
        auto walk = [&](uint32_t u, auto& self) -> void {
          if (state[u] != kUnseen)
            return;
          state[u] = kVisiting;
          const int tgt = rtBuses_[u].target;
          if (tgt >= 0 && static_cast<uint32_t>(tgt) < kMaxBusCount) {
            if (state[static_cast<uint32_t>(tgt)] == kVisiting)
              rtBuses_[u].target = -1; // cyclic edge: rescue to master
            else
              self(static_cast<uint32_t>(tgt), self);
          }
          state[u] = kDone;
        };
        for (uint32_t b = 0; b < kMaxBusCount; ++b)
          walk(b, walk);
      }
      int busOrder[kMaxBusCount];
      int orderLen = 0;
      State state[kMaxBusCount] = {};
      // Post-order over dependency edges: a bus renders after every bus it
      // depends on. Dependencies are (a) its route children (buses that flow
      // into it), (b) the key buses its inserts key on, (c) the sources of
      // sends aimed at it. A dependency sitting in kVisiting is a cycle edge
      // and is dropped (route cycles were already rescued above).
      auto visit = [&](uint32_t u, auto& self) -> void {
        if (state[u] != kUnseen)
          return;
        state[u] = kVisiting;
        for (uint32_t c = 0; c < kMaxBusCount; ++c)
          if (c != u && rtBuses_[c].target == static_cast<int>(u) &&
              state[c] != kVisiting)
            self(c, self);
        if (fxSlot && u < fxSlot->busChains.size())
          for (const FxPluginRT& ins : fxSlot->busChains[u])
            if (ins.scSource >= 0 &&
                static_cast<uint32_t>(ins.scSource) < kMaxBusCount &&
                state[static_cast<uint32_t>(ins.scSource)] != kVisiting)
              self(static_cast<uint32_t>(ins.scSource), self);
        if (sendSlot)
          for (const MixerSendParam& snd : sendSlot->params)
            if (snd.enabled && snd.sourceBus < kMaxBusCount &&
                snd.target == u &&
                state[static_cast<uint32_t>(snd.sourceBus)] != kVisiting)
              self(static_cast<uint32_t>(snd.sourceBus), self);
        state[u] = kDone;
        busOrder[orderLen++] = static_cast<int>(u);
      };
      // Visit every bus (index order) so dangling nodes — e.g. a key bus with
      // no track of its own — still land somewhere in the order.
      for (uint32_t b = 0; b < kMaxBusCount; ++b)
        visit(b, visit);
      busOrderLen_ = orderLen;
      for (int i = 0; i < orderLen; ++i)
        busOrder_[i] = busOrder[i];
      bslot.refs.fetch_sub(1, std::memory_order_release);
    }
    const bool anySolo = anyTrackSolo || anyBusSolo;

    // Automation ("gain"/"volume"/"pan") curves published for this block.
    // Held for the whole per-track mix so the per-sample sampler never touches
    // a reallocated vector; forward+backward stepping keeps interpolation
    // linear without a per-sample search.
    const int as = liveAutomationSlot_.load(std::memory_order_acquire);
    AutomationSlot* aparamSlot = nullptr;
    if (as >= 0 && !automationSlots_[as].params.empty()) {
      aparamSlot = &automationSlots_[as];
      aparamSlot->refs.fetch_add(1, std::memory_order_relaxed);
    }

    // Post-fader effect chains published for this block, held in the mixer fold
    // above so both the bus order compile and the per-track mix read one
    // consistent, live slot.

    // Audio-thread-local lookup for one parameter's curve on one track
    // (rarely non-null). Each track/param pair keeps its own monotonic cursor.
    auto findCurve = [&](uint32_t t, const std::string& param) -> const AutomationParam* {
      if (!aparamSlot)
        return nullptr;
      for (const AutomationParam& a : aparamSlot->params)
        if (a.trackIndex == t && a.active && a.param == param)
          return &a;
      return nullptr;
    };
    // Samples a published curve at `beats` with no per-sample search. Returns
    // `neutral` when there is no active curve; `*idx` is the monotonic cursor
    // (rewinds only when a seek jumps backwards).
    auto sampleCurve = [](const AutomationParam* c, uint32_t* idx,
                          double beats, float neutral) -> float {
      if (!c)
        return neutral;
      const uint32_t n = static_cast<uint32_t>(c->times.size());
      if (n == 0)
        return neutral;
      if (beats <= c->times.front())
        return c->values.front();
      if (beats >= c->times.back())
        return c->values.back();
      while (*idx > 0 && beats < c->times[*idx])
        --*idx; // seek backwards: rewind the cursor
      while (*idx + 1 < n && beats >= c->times[*idx + 1])
        ++*idx;
      const uint32_t i = *idx;
      const double t0 = c->times[i];
      const double t1 = c->times[i + 1];
      const float v0 = c->values[i];
      const float v1 = c->values[i + 1];
      if (t1 <= t0)
        return v0;
      double x = (beats - t0) / (t1 - t0);
      if (x < 0.0)
        x = 0.0;
      else if (x > 1.0)
        x = 1.0;
      return static_cast<float>(v0 + (v1 - v0) * x);
    };

    const double fpb = transport_.framesPerBeat();

    for (uint32_t base = 0; base < frames; base += kMaxBlockFrames) {
      const uint32_t seg = std::min(kMaxBlockFrames, frames - base);
      const uint64_t segPos = songPos + base;
      bool busUsed[kMaxBusCount] = {};
      for (uint32_t b = 0; b < kMaxBusCount; ++b) {
        std::fill(&busScratchL_[b * kMaxBlockFrames],
                  &busScratchL_[b * kMaxBlockFrames] + seg, 0.f);
        std::fill(&busScratchR_[b * kMaxBlockFrames],
                  &busScratchR_[b * kMaxBlockFrames] + seg, 0.f);
      }
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
            midiPlayer_.renderScorePlanar(ss->tracks[t], true, sL, sR, seg,
                                          segPos);
          else
            midiPlayer_.renderScore(ss->tracks[t], true, sL, sL, seg, 1,
                                    segPos);
        }
        if (hasClips) {
          if (outputChannels >= 2)
            clipPlayer_.renderPlanar(cs->tracks[t], sL, sR, seg, segPos);
          else
            clipPlayer_.render(cs->tracks[t], sL, sL, seg, 1, segPos);
        }
        // Smoothed fader + constant-power pan and automation applied in place
        // per sample. Each curve is sampled against the absolute frame
        // position; a monotonic cursor keeps interpolation linear without a
        // per-sample search, rewinding only when a seek jumps backwards.
        const AutomationParam* gainCurve = findCurve(t, "gain");
        const AutomationParam* volCurve = findCurve(t, "volume");
        const AutomationParam* panCurve = findCurve(t, "pan");
        uint32_t gIdx = 0, vIdx = 0, pIdx = 0;
        for (uint32_t n = 0; n < seg; ++n) {
          const float g = rt.gain.tick();
          float gl = rt.panL.tick();
          float gr = rt.panR.tick();
          const double beats =
              fpb > 0.0 ? static_cast<double>(segPos + n) / fpb : 0.0;
          const float ag = sampleCurve(gainCurve, &gIdx, beats, 1.f);
          const float av = sampleCurve(volCurve, &vIdx, beats, 1.f);
          if (panCurve) {
            const float pan =
                std::clamp(sampleCurve(panCurve, &pIdx, beats, 0.f), -1.f, 1.f);
            const auto [pl, pr] = dsp::panLaw(pan);
            gl = pl;
            gr = pr;
          }
          sL[n] *= g * gl * ag * av;
          sR[n] *= g * gr * ag * av;
        }
        // Post-fader inserts: render the track's chain in place. Each instance
        // stages into its own buffers, so in/out aliasing here is safe. A
        // disabled insert is a pass-through (the chain loop simply skips it).
        //
        // FX control-port automation ("fx:<chain>:<port>" curves) is written
        // into the inserts at control rate and the plugin runs right after the
        // control is applied; the plugin reads its control inputs from these
        // buffers at the start of run(), so this changes sound both live and
        // offline (renderToWav shares this exact path). Values track the
        // piecewise-linear curve at a 64-frame sub-block cadence so movement
        // that lands mid-segment is not held back until the next segment.
        const uint32_t fxChainCount =
            (fxSlot && t < fxSlot->chains.size())
                ? static_cast<uint32_t>(fxSlot->chains[t].size())
                : 0;
        struct FxCurve {
          const AutomationParam* p;
          uint32_t chain;
          uint32_t port;
          uint32_t cur;
        };
        FxCurve fxCur[16];
        uint32_t nFxCur = 0;
        if (aparamSlot && fxChainCount > 0) {
          const auto& chain = fxSlot->chains[t];
          for (const AutomationParam& a : aparamSlot->params) {
            if (a.trackIndex != t || !a.active ||
                a.param.size() < 3 || a.param.compare(0, 3, "fx:") != 0)
              continue;
            const FxAutomationParam fa = parseFxAutomation(a.param);
            if (!fa.ok || fa.chainIndex >= fxChainCount)
              continue;
            const FxPluginRT& ins = chain[fa.chainIndex];
            if (!ins.enabled || !ins.handle)
              continue; // bypassed insert: automation is inaudible anyway
            if (nFxCur < 16)
              fxCur[nFxCur++] = {&a, fa.chainIndex, fa.portIndex, 0};
          }
        }
        if (fxSlot && fxChainCount > 0) {
          const auto& chain = fxSlot->chains[t];
          const auto runInsert = [&](uint32_t ci, float* outL, float* outR,
                                     uint32_t len) {
            const FxPluginRT& p = chain[ci];
            if (p.kind == FxPluginRT::Kind::BuiltinEq)
              static_cast<dsp::ParametricEq*>(p.handle)->process(outL, outR,
                                                                  len);
            else
              Lv2Host::run(p.handle, outL, outR, outL, outR, len, true);
          };
          if (nFxCur == 0) {
            // No control automation: run the chain for the whole segment exactly
            // as before (sample-identical to sub-blocking, and identical to the
            // unautomated passes the tests compare against).
            for (uint32_t ci = 0; ci < chain.size(); ++ci)
              if (chain[ci].enabled)
                runInsert(ci, sL, sR, seg);
          } else {
            for (uint32_t sb = 0; sb < seg; sb += kFxControlFrames) {
              const uint32_t sub = std::min(kFxControlFrames, seg - sb);
              const uint64_t subPos = segPos + sb;
              const double beat =
                  fpb > 0.0 ? static_cast<double>(subPos) / fpb : 0.0;
              for (uint32_t ci = 0; ci < chain.size(); ++ci) {
                if (!chain[ci].enabled)
                  continue;
                for (uint32_t k = 0; k < nFxCur; ++k) {
                  if (fxCur[k].chain != ci)
                    continue;
                  const float v = sampleCurve(
                      fxCur[k].p, &fxCur[k].cur, beat,
                      fxCur[k].p->times.empty()
                          ? 0.f
                          : fxCur[k].p->values.front());
                  if (chain[ci].kind == FxPluginRT::Kind::BuiltinEq)
                    static_cast<dsp::ParametricEq*>(chain[ci].handle)
                        ->setPort(fxCur[k].port, v);
                  else
                    Lv2Host::setControl(chain[ci].handle, fxCur[k].port, v);
                }
                runInsert(ci, sL + sb, sR + sb, sub);
              }
            }
          }
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
        // Solo semantics: any solo anywhere (track or bus) switches the mix to
        // solo mode. A track then sounds only when it is soloed itself or its
        // destination bus is the sole soloed bus (a soloed bus pulls all its
        // non-muted routed tracks through); direct-to-master tracks sound only
        // when soloed themselves.
        const int b = rt.busIndex >= 0 && rt.busIndex < static_cast<int>(kMaxBusCount)
                          ? rt.busIndex
                          : -1;
        const bool busSoloed = b >= 0 && rtBuses_[b].solo;
        const bool audible =
            !rt.mute && (!anySolo || rt.solo || busSoloed);
        if (audible && b >= 0) {
          busUsed[b] = true;
          float* const dL = &busScratchL_[b * kMaxBlockFrames];
          float* const dR = &busScratchR_[b * kMaxBlockFrames];
          for (uint32_t n = 0; n < seg; ++n) {
            dL[n] += sL[n];
            dR[n] += sR[n];
          }
        } else if (audible) {
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
        // Parallel sends from this track (Comp19): a post-fader, post-insert
        // tap into a target bus, scaled by the send level. The tap only fires
        // while the source is audibly routed (its own mute/solo gate), so a
        // gated source sends silence; the source keeps its own route entirely.
        if (audible && sendSlot) {
          const size_t nSends =
              std::min<size_t>(sendSlot->params.size(), kMaxSends);
          for (size_t si = 0; si < nSends; ++si) {
            const MixerSendParam& sp = sendSlot->params[si];
            if (!sp.enabled || sp.sourceTrack != t ||
                sp.sourceBus != static_cast<uint8_t>(0xFF) ||
                sp.target >= kMaxBusCount)
              continue;
            const uint32_t trg = sp.target;
            busUsed[trg] = true;
            float* const tL = &busScratchL_[trg * kMaxBlockFrames];
            float* const tR = &busScratchR_[trg * kMaxBlockFrames];
            core::Smoother& lvl = rtSends_[si].level;
            for (uint32_t n = 0; n < seg; ++n) {
              const float lv = lvl.tick();
              tL[n] += sL[n] * lv;
              tR[n] += sR[n] * lv;
            }
          }
        }
      }
      // Sum the used buses in render order: smoothed fader + pan in place,
      // meter the post-gain signal, run the bus insert chain (keyed by an
      // optional sidechain bus), tap out any parallel sends, then route the
      // post-fader, post-chain signal into the bus's target (or the master
      // output when it has no target), gated by mute. Only buses that actually
      // carried audio this segment are touched, so the loop is cheap on the
      // common no-bus path. `busOrder_` (a top-down sort compiled at fold time)
      // guarantees a bus renders after every bus it routes into or keys on.
      const int orderLen = busOrderLen_;
      const int iterCount =
          orderLen > 0 ? orderLen : static_cast<int>(kMaxBusCount);
      for (int oi = 0; oi < iterCount; ++oi) {
        const uint32_t b = orderLen > 0 ? static_cast<uint32_t>(busOrder_[oi])
                                        : static_cast<uint32_t>(oi);
        if (!busUsed[b])
          continue;
        BusRT& brt = rtBuses_[b];
        float* const dL = &busScratchL_[b * kMaxBlockFrames];
        float* const dR = &busScratchR_[b * kMaxBlockFrames];
        for (uint32_t n = 0; n < seg; ++n) {
          const float g = brt.gain.tick();
          const float gl = brt.panL.tick();
          const float gr = brt.panR.tick();
          dL[n] *= g * gl;
          dR[n] *= g * gr;
        }
        brt.meterL.process(dL, seg);
        brt.meterR.process(dR, seg);
        busPeakL_[b].store(brt.meterL.peak(), std::memory_order_relaxed);
        busPeakR_[b].store(brt.meterR.peak(), std::memory_order_relaxed);
        busRmsL_[b].store(brt.meterL.rms(), std::memory_order_relaxed);
        busRmsR_[b].store(brt.meterR.rms(), std::memory_order_relaxed);
        busClipped_[b].store(brt.meterL.clipped() || brt.meterR.clipped(),
                              std::memory_order_relaxed);

        // Bus post-fader insert chain (Comp19). Instances render in place; a
        // sidechain-capable insert (the built-in SC compressor) feeds its
        // detector off another bus's already-rendered post-chain output. The
        // key bus was ordered first, so its segment output is ready here. A
        // key reference outside the valid range keys the insert on itself.
        if (fxSlot && b < fxSlot->busChains.size()) {
          const auto& chain = fxSlot->busChains[b];
          for (uint32_t ci = 0; ci < chain.size(); ++ci) {
            if (!chain[ci].enabled)
              continue;
            const FxPluginRT& p = chain[ci];
            const int sc = p.scSource;
            const bool hasKey =
                sc >= 0 && static_cast<uint32_t>(sc) < kMaxBusCount;
            if (p.kind == FxPluginRT::Kind::BuiltinEq) {
              static_cast<dsp::ParametricEq*>(p.handle)->process(dL, dR, seg);
            } else if (p.kind == FxPluginRT::Kind::BuiltinComp) {
              static_cast<dsp::Compressor*>(p.handle)->process(
                  dL, dR,
                  hasKey ? &busScratchL_[static_cast<uint32_t>(sc) *
                                         kMaxBlockFrames]
                         : dL,
                  hasKey ? &busScratchR_[static_cast<uint32_t>(sc) *
                                         kMaxBlockFrames]
                         : dR,
                  seg);
            } else {
              Lv2Host::run(p.handle, dL, dR, dL, dR, seg, true);
            }
          }
        }

        if (!brt.mute) {
          // Parallel sends from this bus: a post-fader, post-chain tap into the
          // target bus that renders later in the order (or next block's fold
          // marks it used the moment this block's tap lands).
          if (sendSlot) {
            const size_t nSends =
                std::min<size_t>(sendSlot->params.size(), kMaxSends);
            for (size_t si = 0; si < nSends; ++si) {
              const MixerSendParam& sp = sendSlot->params[si];
              if (!sp.enabled || sp.sourceBus != b ||
                  sp.sourceTrack != static_cast<uint8_t>(0xFF) ||
                  sp.target >= kMaxBusCount)
                continue;
              const uint32_t trg = sp.target;
              busUsed[trg] = true;
              float* const tL = &busScratchL_[trg * kMaxBlockFrames];
              float* const tR = &busScratchR_[trg * kMaxBlockFrames];
              core::Smoother& lvl = rtSends_[si].level;
              for (uint32_t n = 0; n < seg; ++n) {
                const float lv = lvl.tick();
                tL[n] += dL[n] * lv;
                tR[n] += dR[n] * lv;
              }
            }
          }
          const int tgt = brt.target;
          if (tgt >= 0 && tgt < static_cast<int>(kMaxBusCount)) {
            busUsed[tgt] = true;
            float* const tL = &busScratchL_[tgt * kMaxBlockFrames];
            float* const tR = &busScratchR_[tgt * kMaxBlockFrames];
            for (uint32_t n = 0; n < seg; ++n) {
              tL[n] += dL[n];
              tR[n] += dR[n];
            }
          } else {
            if (outputChannels >= 2) {
              for (uint32_t n = 0; n < seg; ++n) {
                output[(base + n) * outputChannels] += dL[n];
                output[(base + n) * outputChannels + 1] += dR[n];
              }
            } else {
              for (uint32_t n = 0; n < seg; ++n)
                output[base + n] += 0.5f * (dL[n] + dR[n]);
            }
          }
        }
      }
    }
    if (cs)
      cs->refs.fetch_sub(1, std::memory_order_release);
    if (ss)
      ss->refs.fetch_sub(1, std::memory_order_release);
    if (aparamSlot)
      aparamSlot->refs.fetch_sub(1, std::memory_order_release);
    if (sendSlot)
      sendSlot->refs.fetch_sub(1, std::memory_order_release);
    if (fxSlot)
      fxSlot->refs.fetch_sub(1, std::memory_order_release);

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
    // the published buffer has been consumed. Pause holds the position at the
    // exact frame (the preview resumes where it stopped); a fresh publish or
    // stop rewinds to the top.
    if (previewActive_) {
      const int ps = livePreviewSlot_.load(std::memory_order_acquire);
      PreviewSlot* pv =
          (ps >= 0 && !previewSlots_[ps].samples.empty()) ? &previewSlots_[ps]
                                                          : nullptr;
      if (!pv) {
        previewActive_ = false;
        previewPosFrames_ = 0;
      } else {
        pv->refs.fetch_add(1, std::memory_order_relaxed);
        const uint32_t ch = previewChannels_;
        const uint32_t outCh = outputChannels > 1 ? 2 : 1;
        const uint64_t totalF =
            static_cast<uint64_t>(pv->samples.size()) / ch;
        const float gain = previewGain_;
        const uint32_t edge =
            static_cast<uint32_t>(std::min<uint64_t>(256, totalF / 2));
        uint64_t pos = previewPosFrames_;
        if (!previewPaused_) {
          for (uint32_t i = 0; i < frames; ++i) {
            if (pos >= totalF) {
              previewActive_ = false;
              previewPosFrames_ = 0;
              break;
            }
            const size_t s0 = static_cast<size_t>(pos) * ch;
            float l = pv->samples[s0];
            float r = ch == 2 ? pv->samples[s0 + 1] : l;
            if (outCh == 1)
              l = r = 0.5f * (l + r);
            float env = 1.f;
            const uint64_t idx = pos;
            if (idx < edge)
              env = static_cast<float>(idx + 1) / edge;
            else if (idx >= totalF - edge)
              env = static_cast<float>(totalF - idx) / edge;
            if (outputChannels >= 2) {
              output[i * outputChannels] += l * env * gain;
              output[i * outputChannels + 1] += r * env * gain;
            } else {
              output[i] += 0.5f * (l + r) * env * gain;
            }
            ++pos;
          }
        }
        previewPosFrames_ = pos;
        pv->refs.fetch_sub(1, std::memory_order_release);
      }
    }
    previewActiveSnapshot_.store(previewActive_, std::memory_order_relaxed);
    previewPosSnapshot_.store(previewPosFrames_, std::memory_order_relaxed);
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

  // Master bus gain (smoothed to avoid clicks) and the brickwall limiter.
  // The limiter is the last gain stage before the meters: live and offline
  // renderings share it, so no output sample can exceed its ceiling and the
  // meters always reflect the delivered signal.
  for (uint32_t i = 0; i < frames; ++i) {
    const float g = masterSmoother_.tick();
for (uint32_t c = 0; c < outputChannels; ++c)
      output[i * outputChannels + c] *= g;
  }
  if (limiterOn_)
    limiter_.process(output, frames, outputChannels);

  if (outputChannels >= 2) {
    meterL_.processInterleavedTruePeak(output, outputChannels, 0, frames);
    meterR_.processInterleavedTruePeak(output, outputChannels, 1, frames);
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