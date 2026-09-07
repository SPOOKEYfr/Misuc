#include <misuc/engine/audio/PipeWireBackend.h>

#include <cstring>
#include <thread>

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/pod/vararg.h>
#include <spa/utils/hook.h>
#include <spa/utils/string.h>

namespace misuc::engine {

namespace {

constexpr uint32_t kConnectTimeoutMs = 8000;

AudioDeviceInfo makeDevice(uint32_t id, const struct spa_dict* props, bool input) {
  AudioDeviceInfo info;
  info.id = std::to_string(id);
  info.isInput = input;
  const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  info.name = desc ? desc : (name ? name : ("Device " + std::to_string(id)));
  info.api = spa_dict_lookup(props, PW_KEY_DEVICE_API) ?: "";
  info.description = name ? name : info.name;
  return info;
}

// ---- device enumeration (unchanged) ----------------------------------------

struct EnumCtx {
  std::vector<AudioDeviceInfo>* out;
  bool input;
  bool done = false;
  std::mutex mutex;
  std::condition_variable cv;
  struct pw_thread_loop* loop = nullptr;
  struct pw_context* context = nullptr;
  struct pw_core* core = nullptr;
  struct pw_registry* registry = nullptr;
  struct spa_hook coreHook{};
  struct spa_hook registryHook{};
};

void enumOnGlobal(void* data, uint32_t id, uint32_t, const char* type,
                  uint32_t, const struct spa_dict* props) {
  auto* ctx = static_cast<EnumCtx*>(data);
  if (!type || !props)
    return;
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  const char* mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!mc)
    return;
  const bool isSink = std::strcmp(mc, "Audio/Sink") == 0 ||
                      std::strcmp(mc, "Audio/Sink/Virtual") == 0;
  const bool isSource = std::strcmp(mc, "Audio/Source") == 0 ||
                        std::strcmp(mc, "Audio/Source/Virtual") == 0;
  if (!isSink && !isSource)
    return;
  if (spa_dict_lookup(props, "node.monitor") != nullptr)
    return;
  if (isSource != ctx->input)
    return;
  ctx->out->push_back(makeDevice(id, props, isSource));
}

void enumOnGlobalRemove(void*, uint32_t) {}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
constexpr struct pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = enumOnGlobal,
    .global_remove = enumOnGlobalRemove,
};
#pragma GCC diagnostic pop

void enumOnCoreInfo(void*, const struct pw_core_info*) {}
void enumOnCoreDone(void* data, uint32_t, int) {
  auto* ctx = static_cast<EnumCtx*>(data);
  std::lock_guard<std::mutex> lk(ctx->mutex);
  ctx->done = true;
  ctx->cv.notify_all();
}
constexpr struct pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS, .info = enumOnCoreInfo, .done = enumOnCoreDone,
};

// ---- streaming ------------------------------------------------------------

struct StreamCtx {
  PipeWireBackend* self;
  bool input;
};

void streamOnProcess(void* data) {
  auto* ctx = static_cast<StreamCtx*>(data);
  if (ctx->input)
    ctx->self->onInputProcess();
  else
    ctx->self->onOutputProcess();
}

void streamOnState(void* data, enum pw_stream_state, enum pw_stream_state new_state,
                   const char* error) {
  auto* ctx = static_cast<StreamCtx*>(data);
  if (error)
    ctx->self->setStreamError(error);
  ctx->self->onStateChanged(ctx->input, static_cast<int>(new_state), error);
  std::unique_lock<std::mutex> lk(ctx->self->streamMutex_);
  ctx->self->stateCv_.notify_all();
}

void streamOnParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
  auto* ctx = static_cast<StreamCtx*>(data);
  ctx->self->onParamChanged(ctx->input, id, param);
}

void streamOnIoChanged(void*, uint32_t, void*, uint32_t) {}
void streamOnDrained(void*) {}
void streamOnControlInfo(void*, uint32_t, const struct pw_stream_control*) {}
void streamOnAddBuffer(void*, struct pw_buffer*) {}
void streamOnRemoveBuffer(void*, struct pw_buffer*) {}

constexpr struct pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = streamOnState,
    .control_info = streamOnControlInfo,
    .io_changed = streamOnIoChanged,
    .param_changed = streamOnParamChanged,
    .add_buffer = streamOnAddBuffer,
    .remove_buffer = streamOnRemoveBuffer,
    .process = streamOnProcess,
    .drained = streamOnDrained,
    .command = nullptr,
    .trigger_done = nullptr,
};

// ---- helpers ---------------------------------------------------------------

bool buildOutFormat(const AudioConfig& cfg, uint32_t& outCh,
                    const struct spa_pod*& pod,
                    struct spa_pod_builder& b, uint8_t* buf, size_t bufSz) {
  spa_pod_builder_init(&b, buf, bufSz);
  struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
      .format = SPA_AUDIO_FORMAT_F32,
      .rate = cfg.sampleRate ? cfg.sampleRate : 48000,
      .channels = outCh,
      .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  pod = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  return pod != nullptr;
}

bool buildInFormat(uint32_t rate, uint32_t inCh,
                   const struct spa_pod*& pod,
                   struct spa_pod_builder& b, uint8_t* buf, size_t bufSz) {
  spa_pod_builder_init(&b, buf, bufSz);
  struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
      .format = SPA_AUDIO_FORMAT_F32,
      .rate = rate,
      .channels = inCh,
      .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  pod = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
  return pod != nullptr;
}

} // namespace

// ---- public ----------------------------------------------------------------

PipeWireBackend::PipeWireBackend() { pw_init(nullptr, nullptr); }
PipeWireBackend::~PipeWireBackend() { stop(); }

std::string PipeWireBackend::backendName() const { return "PipeWire"; }
std::vector<uint32_t> PipeWireBackend::supportedSampleRates() const {
  return {22050, 32000, 44100, 48000, 88200, 96000, 192000};
}
std::vector<uint32_t> PipeWireBackend::supportedBufferSizes() const {
  return {16, 32, 64, 128, 256, 512, 1024, 2048};
}

std::vector<AudioDeviceInfo> PipeWireBackend::enumerateDevices(bool input) {
  std::vector<AudioDeviceInfo> result;
  EnumCtx ctx;
  ctx.out = &result;
  ctx.input = input;

  ctx.loop = pw_thread_loop_new("misuc-enum", nullptr);
  if (!ctx.loop) return result;
  ctx.context = pw_context_new(pw_thread_loop_get_loop(ctx.loop), nullptr, 0);
  if (!ctx.context) {
    pw_thread_loop_destroy(ctx.loop);
    return result;
  }
  pw_thread_loop_lock(ctx.loop);
  ctx.core = pw_context_connect(ctx.context, nullptr, 0);
  if (!ctx.core) {
    pw_thread_loop_unlock(ctx.loop);
    pw_context_destroy(ctx.context);
    pw_thread_loop_destroy(ctx.loop);
    return result;
  }
  pw_core_add_listener(ctx.core, &ctx.coreHook, &kCoreEvents, &ctx);
  ctx.registry = pw_core_get_registry(ctx.core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(ctx.registry, &ctx.registryHook, &kRegistryEvents, &ctx);
  pw_core_sync(ctx.core, PW_ID_CORE, 0);
  pw_thread_loop_start(ctx.loop);
  pw_thread_loop_unlock(ctx.loop);

  {
    std::unique_lock<std::mutex> lk(ctx.mutex);
    if (!ctx.cv.wait_for(lk, std::chrono::seconds(3), [&] { return ctx.done; }))
      result.clear();
  }
  pw_core_disconnect(ctx.core);
  pw_context_destroy(ctx.context);
  pw_thread_loop_destroy(ctx.loop);
  return result;
}

bool PipeWireBackend::open(const AudioConfig& config, std::string& error) {
  if (running_.load()) {
    error = "The audio engine is already running.";
    return false;
  }
  config_ = config;
  channels_ = config.channels == 0 ? 2 : config.channels;
  inputChannels_ = config.inputChannels == 0 ? 2 : config.inputChannels;
  sampleRate_ = config.sampleRate == 0 ? 48000 : config.sampleRate;
  if (channels_ > kMaxScratchChannels) {
    error = "Unsupported output channel count: " + std::to_string(channels_);
    return false;
  }
  if (inputChannels_ > kMaxScratchChannels) {
    error = "Unsupported input channel count: " + std::to_string(inputChannels_);
    return false;
  }
  std::memset(outScratch_, 0, sizeof outScratch_);
  std::memset(inScratch_, 0, sizeof inScratch_);
  return true;
}

bool PipeWireBackend::start(AudioRenderer& renderer) {
  if (running_.load())
    return true;
  renderer_ = &renderer;
  stateReachedOut_.store(false);
  stateReachedIn_.store(false);
  streamWasError_ = false;
  outState_.store(0);
  inState_.store(0);
  outSlot_ = {};
  inSlot_ = {};
  negotiatedInputChannels_.store(0);

  const bool wantCapture = !config_.inputDeviceId.empty();

  loop_ = pw_thread_loop_new("misuc-audio", nullptr);
  if (!loop_) {
    setStreamError("Could not create the audio thread.");
    return false;
  }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) {
    setStreamError("Could not create the audio context.");
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    return false;
  }

  pw_thread_loop_lock(loop_);
  core_ = pw_context_connect(context_, nullptr, 0);
  if (!core_) {
    setStreamError("Could not connect to the PipeWire daemon.");
    pw_thread_loop_unlock(loop_);
    stop();
    return false;
  }

  const enum pw_stream_flags outFlags = static_cast<enum pw_stream_flags>(
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE |
      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

  // Output stream -----------------------------------------------------------
  {
    auto* ctx = new StreamCtx{this, false};
    outSlot_.ctx = ctx;
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "misuc",
        PW_KEY_NODE_NAME, "misuc-playback", nullptr);
    if (!config_.deviceId.empty())
      pw_properties_set(props, PW_KEY_TARGET_OBJECT, config_.deviceId.c_str());

    uint8_t podBuf[512];
    struct spa_pod_builder b{};
    const struct spa_pod* params[1];
    if (!buildOutFormat(config_, channels_, params[0], b, podBuf, sizeof podBuf)) {
      setStreamError("Could not build output format.");
      pw_thread_loop_unlock(loop_);
      stop();
      return false;
    }

    outSlot_.stream = pw_stream_new(core_, "misuc-playback", props);
    if (!outSlot_.stream) {
      setStreamError("Could not create output stream.");
      pw_thread_loop_unlock(loop_);
      stop();
      return false;
    }
    outSlot_.hook = new spa_hook();
    outSlot_.channels = channels_;
    pw_stream_add_listener(outSlot_.stream, outSlot_.hook, &kStreamEvents, ctx);

    if (pw_stream_connect(outSlot_.stream, PW_DIRECTION_OUTPUT,
                          PW_ID_ANY, outFlags, params, 1) < 0) {
      setStreamError("The output device rejected the connection.");
      pw_thread_loop_unlock(loop_);
      stop();
      return false;
    }
  }

  // Capture stream ----------------------------------------------------------
  if (wantCapture) {
    auto* ctx = new StreamCtx{this, true};
    inSlot_.ctx = ctx;
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_APP_NAME, "misuc",
        PW_KEY_NODE_NAME, "misuc-capture", nullptr);
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, config_.inputDeviceId.c_str());

    uint8_t podBuf[512];
    struct spa_pod_builder b{};
    const struct spa_pod* params[1];
    if (!buildInFormat(sampleRate_, inputChannels_, params[0], b, podBuf, sizeof podBuf)) {
      setStreamError("Could not build input format.");
      pw_thread_loop_unlock(loop_);
      stop();
      return false;
    }

    inSlot_.stream = pw_stream_new(core_, "misuc-capture", props);
    if (!inSlot_.stream) {
      setStreamError("Could not create input stream.");
      pw_thread_loop_unlock(loop_);
      stop();
      return false;
    }
    inSlot_.hook = new spa_hook();
    inSlot_.channels = inputChannels_;
    pw_stream_add_listener(inSlot_.stream, inSlot_.hook, &kStreamEvents, ctx);

    if (pw_stream_connect(inSlot_.stream, PW_DIRECTION_INPUT,
                          PW_ID_ANY, outFlags, params, 1) < 0) {
      setStreamError("The input device rejected the connection.");
      pw_thread_loop_unlock(loop_);
      stop();
      return false;
    }
  }

  // Start and wait for the output stream to reach streaming state.
  pw_thread_loop_start(loop_);
  loopStarted_ = true;
  pw_thread_loop_unlock(loop_);

  {
    std::unique_lock<std::mutex> lk(streamMutex_);
    const bool outReady = stateCv_.wait_for(
        lk, std::chrono::milliseconds(kConnectTimeoutMs),
        [&] { return stateReachedOut_.load(); });
    if (!outReady) {
      setStreamError("Timed out connecting the output device.");
      stop();
      return false;
    }
    if (outState_.load() == static_cast<int>(PW_STREAM_STATE_ERROR)) {
      std::string err = streamErrorLocked();
      stop();
      setStreamError(err);
      return false;
    }
  }

  if (wantCapture) {
    std::unique_lock<std::mutex> lk(streamMutex_);
    const bool inReady = stateCv_.wait_for(
        lk, std::chrono::milliseconds(kConnectTimeoutMs),
        [&] { return stateReachedIn_.load(); });
    if (!inReady) {
      setStreamError("Timed out connecting the input device.");
      stop();
      return false;
    }
    if (inState_.load() == static_cast<int>(PW_STREAM_STATE_ERROR)) {
      std::string err = streamErrorLocked();
      stop();
      setStreamError(err);
      return false;
    }
  }

  if (negotiatedRate_.load() == 0)
    negotiatedRate_.store(sampleRate_);
  if (negotiatedChannels_.load() == 0)
    negotiatedChannels_.store(channels_);

  pw_thread_loop_lock(loop_);
  pw_stream_set_active(outSlot_.stream, true);
  if (inSlot_.stream)
    pw_stream_set_active(inSlot_.stream, true);
  pw_thread_loop_unlock(loop_);

  running_.store(true);
  lastProcessTimeNs_.store(-1);
  lastPosition_.store(0);
  outDelayMs_.store(0.0);
  inDelayMs_.store(0.0);
  return true;
}

// ---- realtime callbacks ----------------------------------------------------

void PipeWireBackend::onOutputProcess() noexcept {
  if (!renderer_)
    return;
  struct pw_buffer* buf = pw_stream_dequeue_buffer(outSlot_.stream);
  if (!buf)
    return;
  struct spa_buffer* sbuf = buf->buffer;
  if (!sbuf || !sbuf->datas || !sbuf->datas[0].data) {
    pw_stream_queue_buffer(outSlot_.stream, buf);
    return;
  }

  const uint32_t nData = sbuf->n_datas;
  const uint32_t bps = sizeof(float);
  const bool planar = nData > 1 && nData >= channels_;
  uint32_t frames = planar
      ? sbuf->datas[0].maxsize / bps
      : sbuf->datas[0].maxsize / (channels_ * bps);
  if (frames > kMaxScratchFrames)
    frames = static_cast<uint32_t>(kMaxScratchFrames);
  if (frames == 0) {
    pw_stream_queue_buffer(outSlot_.stream, buf);
    return;
  }

  // Xrun detection via stream timestamp gaps.
  {
    struct pw_time t{};
    if (pw_stream_get_time_n(outSlot_.stream, &t, sizeof t) == 0) {
      outDelayMs_.store(static_cast<double>(t.delay) * 1000.0 /
                            static_cast<double>(t.rate.denom),
                        std::memory_order_relaxed);
      const int64_t now = t.now;
      const int64_t prev = lastProcessTimeNs_.load(std::memory_order_relaxed);
      if (prev > 0 && now > prev) {
        const double durUs =
            static_cast<double>(frames) * 1e6 / static_cast<double>(sampleRate_);
        const double gapUs = static_cast<double>(now - prev) / 1000.0;
        if (gapUs > durUs * 3.0)
          xruns_.fetch_add(1, std::memory_order_relaxed);
      }
      lastProcessTimeNs_.store(now, std::memory_order_relaxed);
    }
  }

  const uint64_t position = lastPosition_.load(std::memory_order_relaxed);
  const float* inPtr = nullptr;
  uint32_t inCh = 0;
  if (inSlot_.stream && inSlot_.channels > 0) {
    inPtr = &inScratch_[0][0];
    inCh = inSlot_.channels;
  }

  if (!planar) {
    float* out = static_cast<float*>(sbuf->datas[0].data);
    renderer_->process(inPtr, inCh, out, channels_, frames, position, sampleRate_);
    sbuf->datas[0].chunk->stride = channels_ * bps;
    sbuf->datas[0].chunk->size = frames * channels_ * bps;
  } else {
    for (uint32_t c = 0; c < channels_; ++c)
      std::memset(outScratch_[c], 0, frames * bps);
    renderer_->process(inPtr, inCh,
                       reinterpret_cast<float*>(outScratch_), channels_,
                       frames, position, sampleRate_);
    for (uint32_t c = 0; c < channels_; ++c) {
      float* planeOut = static_cast<float*>(sbuf->datas[c].data);
      const float* src = outScratch_[c];
      for (uint32_t i = 0; i < frames; ++i)
        planeOut[i] = src[i * channels_];
      sbuf->datas[c].chunk->stride = bps;
      sbuf->datas[c].chunk->size = frames * bps;
    }
  }

  lastPosition_.fetch_add(frames, std::memory_order_relaxed);
  framesProcessed_.fetch_add(frames, std::memory_order_relaxed);
  negotiatedBuffer_.store(frames, std::memory_order_relaxed);
  pw_stream_queue_buffer(outSlot_.stream, buf);
}

void PipeWireBackend::onInputProcess() noexcept {
  if (!inSlot_.stream)
    return;
  struct pw_buffer* buf = pw_stream_dequeue_buffer(inSlot_.stream);
  if (!buf)
    return;
  struct spa_buffer* sbuf = buf->buffer;
  if (!sbuf || !sbuf->datas || !sbuf->datas[0].data) {
    pw_stream_queue_buffer(inSlot_.stream, buf);
    return;
  }

  const uint32_t nData = sbuf->n_datas;
  const uint32_t bps = sizeof(float);
  const bool planar = nData > 1 && nData >= inSlot_.channels;
  const uint32_t inCh = inSlot_.channels;
  uint32_t frames = planar
      ? sbuf->datas[0].maxsize / bps
      : sbuf->datas[0].maxsize / (inCh * bps);
  if (frames > kMaxScratchFrames)
    frames = static_cast<uint32_t>(kMaxScratchFrames);

  // Snapshot input delay.
  {
    struct pw_time t{};
    if (pw_stream_get_time_n(inSlot_.stream, &t, sizeof t) == 0)
      inDelayMs_.store(static_cast<double>(t.delay) * 1000.0 /
                           static_cast<double>(t.rate.denom),
                       std::memory_order_relaxed);
  }

  // Copy into the interleaved inScratch_ buffer so the output process can
  // read it without cross-stream locking.
  if (!planar) {
    const float* src = static_cast<const float*>(sbuf->datas[0].data);
    const uint32_t n = frames * inCh;
    for (uint32_t i = 0; i < n; ++i)
      inScratch_[0][i] = src[i];
  } else {
    // De-interleave planar to interleaved scratch.
    for (uint32_t c = 0; c < inCh; ++c) {
      const float* plane = static_cast<const float*>(sbuf->datas[c].data);
      for (uint32_t i = 0; i < frames; ++i)
        inScratch_[c][i] = plane[i];
    }
  }

  pw_stream_queue_buffer(inSlot_.stream, buf);
}

void PipeWireBackend::onParamChanged(bool input, uint32_t id, const void* param) noexcept {
  if (id != SPA_PARAM_Format || !param)
    return;
  struct spa_audio_info_raw raw{};
  if (spa_format_audio_raw_parse(static_cast<const struct spa_pod*>(param), &raw) < 0)
    return;
  if (raw.rate != 0)
    negotiatedRate_.store(raw.rate);
  if (input) {
    if (raw.channels != 0) {
      negotiatedInputChannels_.store(raw.channels);
      inSlot_.channels = raw.channels;
    }
  } else {
    if (raw.channels != 0) {
      negotiatedChannels_.store(raw.channels);
      outSlot_.channels = raw.channels;
    }
  }
}

void PipeWireBackend::onStateChanged(bool input, int state, const char* error) noexcept {
  if (input) {
    inState_.store(state);
    if (static_cast<pw_stream_state>(state) == PW_STREAM_STATE_ERROR) {
      running_.store(false);
      streamWasError_ = true;
    }
    stateReachedIn_.store(true);
  } else {
    outState_.store(state);
    if (static_cast<pw_stream_state>(state) == PW_STREAM_STATE_ERROR) {
      running_.store(false);
      streamWasError_ = true;
    }
    stateReachedOut_.store(true);
  }
}

void PipeWireBackend::setStreamError(const std::string& error) noexcept {
  std::lock_guard<std::mutex> lk(streamMutex_);
  streamError_ = error;
}

std::string PipeWireBackend::streamErrorLocked() const {
  std::lock_guard<std::mutex> lk(streamMutex_);
  return streamError_;
}

// ---- teardown (lock-free to avoid the pw_thread_loop_stop deadlock) --------

void PipeWireBackend::stop() noexcept {
  running_.store(false);
  auto destroySlot = [](StreamSlot& s) {
    if (s.stream) { pw_stream_destroy(s.stream); s.stream = nullptr; }
    if (s.hook)   { delete s.hook; s.hook = nullptr; }
    if (s.ctx)    { delete static_cast<StreamCtx*>(s.ctx); s.ctx = nullptr; }
  };
  if (loop_) {
    pw_thread_loop_lock(loop_);
    if (outSlot_.stream) pw_stream_set_active(outSlot_.stream, false);
    if (inSlot_.stream)  pw_stream_set_active(inSlot_.stream, false);
    pw_thread_loop_unlock(loop_);
    if (loopStarted_) {
      pw_thread_loop_stop(loop_);
      loopStarted_ = false;
    }
  }
  destroySlot(outSlot_);
  destroySlot(inSlot_);
  if (core_)    { pw_core_disconnect(core_); core_ = nullptr; }
  if (context_) { pw_context_destroy(context_); context_ = nullptr; }
  if (loop_)    { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
  renderer_ = nullptr;
  running_.store(false);
}

bool PipeWireBackend::isRunning() const { return running_.load(); }

BackendStatus PipeWireBackend::status() const {
  BackendStatus s;
  s.running = running_.load();
  s.framesProcessed = framesProcessed_.load();
  s.xruns = xruns_.load();
  s.negotiatedRate = negotiatedRate_.load();
  s.negotiatedChannels = negotiatedChannels_.load();
  s.negotiatedInputChannels = negotiatedInputChannels_.load();
  s.negotiatedBuffer = negotiatedBuffer_.load();
  if (running_.load())
    s.error = streamErrorLocked();
  return s;
}

LatencyInfo PipeWireBackend::latency() const {
  LatencyInfo l;
  const uint32_t rate = negotiatedRate_.load() ? negotiatedRate_.load() : sampleRate_;
  const uint32_t buf = negotiatedBuffer_.load() ? negotiatedBuffer_.load()
                                                : config_.bufferSize;
  l.bufferMs = static_cast<double>(buf) * 1000.0 / rate;
  l.outputMs = l.bufferMs + outDelayMs_.load(std::memory_order_relaxed);
  l.inputMs = l.bufferMs + inDelayMs_.load(std::memory_order_relaxed);
  l.roundTripMs = l.outputMs + l.inputMs;
  return l;
}

} // namespace misuc::engine