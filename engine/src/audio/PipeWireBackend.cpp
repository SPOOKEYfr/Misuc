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

// ---- node enumeration -----------------------------------------------------

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

void enumOnGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
                  uint32_t version, const struct spa_dict* props) {
  (void)permissions; (void)version;
  auto* ctx = static_cast<EnumCtx*>(data);
  if (type == nullptr || props == nullptr)
    return;
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  const char* mc = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (mc == nullptr)
    return;
  const bool isSink = std::strcmp(mc, "Audio/Sink") == 0 ||
                      std::strcmp(mc, "Audio/Sink/Virtual") == 0;
  const bool isSource = std::strcmp(mc, "Audio/Source") == 0 ||
                        std::strcmp(mc, "Audio/Source/Virtual") == 0;
  if (!isSink && !isSource)
    return;
  if (spa_dict_lookup(props, "node.monitor") != nullptr)
    return;
  const bool nodeInput = isSource;
  if (nodeInput != ctx->input)
    return;
  ctx->out->push_back(makeDevice(id, props, nodeInput));
}

void enumOnGlobalRemove(void*, uint32_t) {}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
constexpr struct pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = enumOnGlobal,
    .global_remove = enumOnGlobalRemove,
};

void enumOnCoreInfo(void*, const struct pw_core_info*) {}

void enumOnCoreDone(void* data, uint32_t, int) {
  auto* ctx = static_cast<EnumCtx*>(data);
  std::lock_guard<std::mutex> lk(ctx->mutex);
  ctx->done = true;
  ctx->cv.notify_all();
}

constexpr struct pw_core_events kCoreEvents = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = enumOnCoreInfo,
    .done = enumOnCoreDone,
};
#pragma GCC diagnostic pop

// ---- streaming ------------------------------------------------------------

struct StreamCtx {
  PipeWireBackend* self;
};

void streamOnProcess(void* data) {
  auto* ctx = static_cast<StreamCtx*>(data);
  ctx->self->processCallback();
}

void streamOnState(void* data, enum pw_stream_state /*old_state*/,
                   enum pw_stream_state new_state, const char* error) {
  auto* ctx = static_cast<StreamCtx*>(data);
  if (error != nullptr)
    ctx->self->setStreamError(error);
  ctx->self->onStateChanged(static_cast<int>(new_state), error);
  std::unique_lock<std::mutex> lk(ctx->self->streamMutex_);
  ctx->self->stateCv_.notify_all();
}

void streamOnParamChanged(void* data, uint32_t id, const struct spa_pod* param) {
  auto* ctx = static_cast<StreamCtx*>(data);
  ctx->self->onParamChanged(id, param);
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

} // namespace

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
  if (ctx.loop == nullptr)
    return result;
  ctx.context = pw_context_new(pw_thread_loop_get_loop(ctx.loop), nullptr, 0);
  if (ctx.context == nullptr) {
    pw_thread_loop_destroy(ctx.loop);
    return result;
  }
  pw_thread_loop_lock(ctx.loop);
  ctx.core = pw_context_connect(ctx.context, nullptr, 0);
  if (ctx.core == nullptr) {
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
  // Teardown must run WITHOUT holding the thread-loop lock; stopping the loop
  // from the same thread that locked it deadlocks forever.
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
  input_ = config.input;
  channels_ = config.channels == 0 ? 2 : config.channels;
  sampleRate_ = config.sampleRate == 0 ? 48000 : config.sampleRate;
  if (channels_ > kMaxScratchChannels) {
    error = "Unsupported channel count: " + std::to_string(channels_);
    return false;
  }
  std::memset(scratch_, 0, sizeof scratch_);
  return true;
}

bool PipeWireBackend::start(AudioRenderer& renderer) {
  if (running_.load())
    return true;
  renderer_ = &renderer;
  stateReached_.store(false);
  streamWasError_ = false;
  streamState_.store(0);

  loop_ = pw_thread_loop_new("misuc-audio", nullptr);
  if (loop_ == nullptr) {
    setStreamError("Could not create the audio thread.");
    return false;
  }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (context_ == nullptr) {
    setStreamError("Could not create the audio context.");
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
    return false;
  }

  auto* ctx = new StreamCtx{this};
  streamCtx_ = ctx;

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, input_ ? "Capture" : "Playback",
      PW_KEY_MEDIA_ROLE, "Music",
      PW_KEY_APP_NAME, "misuc",
      PW_KEY_NODE_NAME, input_ ? "misuc-capture" : "misuc-playback",
      nullptr);
  if (!config_.deviceId.empty())
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, config_.deviceId.c_str());

  struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
      .format = SPA_AUDIO_FORMAT_F32,
      .rate = sampleRate_,
      .channels = channels_,
      .position = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});

  uint8_t podBuffer[512];
  struct spa_pod_builder b{};
  spa_pod_builder_init(&b, podBuffer, sizeof podBuffer);
  const struct spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  const enum pw_stream_flags flags = static_cast<enum pw_stream_flags>(
      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE |
      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

  pw_thread_loop_lock(loop_);
  core_ = pw_context_connect(context_, nullptr, 0);
  if (core_ == nullptr) {
    setStreamError("Could not connect to the PipeWire daemon. Is pipewire running?");
    pw_thread_loop_unlock(loop_);
    stop();
    return false;
  }

  stream_ = pw_stream_new(core_, input_ ? "misuc-capture" : "misuc-playback", props);
  if (stream_ == nullptr) {
    setStreamError("Could not create the audio stream.");
    pw_thread_loop_unlock(loop_);
    stop();
    return false;
  }

  streamHook_ = new spa_hook();
  pw_stream_add_listener(stream_, streamHook_, &kStreamEvents, ctx);

  if (pw_stream_connect(stream_, input_ ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                        PW_ID_ANY, flags, params, 1) < 0) {
    setStreamError("The audio device rejected the connection.");
    pw_thread_loop_unlock(loop_);
    stop();
    return false;
  }
  pw_thread_loop_start(loop_);
  loopStarted_ = true;
  pw_thread_loop_unlock(loop_);

  {
    std::unique_lock<std::mutex> lk(streamMutex_);
    if (!stateCv_.wait_for(lk, std::chrono::milliseconds(kConnectTimeoutMs),
                           [&] { return stateReached_.load(); })) {
      setStreamError("Timed out while connecting to the audio device.");
      stop();
      return false;
    }
  }
  if (streamState_.load() == static_cast<int>(PW_STREAM_STATE_ERROR)) {
    std::string err = streamErrorLocked();
    stop();
    setStreamError(err);
    return false;
  }

  if (negotiatedRate_.load() == 0)
    negotiatedRate_.store(sampleRate_);
  if (negotiatedChannels_.load() == 0)
    negotiatedChannels_.store(channels_);

  pw_thread_loop_lock(loop_);
  pw_stream_set_active(stream_, true);
  pw_thread_loop_unlock(loop_);

  running_.store(true);
  lastProcessTimeNs_.store(-1);
  lastPosition_.store(0);
  return true;
}

void PipeWireBackend::processCallback() noexcept {
  if (!renderer_)
    return;
  struct pw_buffer* buf = pw_stream_dequeue_buffer(stream_);
  if (buf == nullptr)
    return;
  struct spa_buffer* sbuf = buf->buffer;
  if (sbuf == nullptr || sbuf->datas == nullptr || sbuf->datas[0].data == nullptr) {
    pw_stream_queue_buffer(stream_, buf);
    return;
  }

  const uint32_t nData = sbuf->n_datas;
  const uint32_t bytesPerSample = sizeof(float);
  const bool planar = nData > 1 && nData >= channels_;

  uint32_t frames = planar ? sbuf->datas[0].maxsize / bytesPerSample
                           : sbuf->datas[0].maxsize / (channels_ * bytesPerSample);
  if (frames > kMaxScratchFrames)
    frames = static_cast<uint32_t>(kMaxScratchFrames);
  if (frames == 0) {
    pw_stream_queue_buffer(stream_, buf);
    return;
  }

  {
    struct pw_time t{};
    int res = pw_stream_get_time_n(stream_, &t, sizeof(t));
    const int64_t now = res == 0 ? t.now : 0;
    const int64_t prev = lastProcessTimeNs_.load(std::memory_order_relaxed);
    if (prev > 0 && now > prev) {
      const double durationUs =
          static_cast<double>(frames) * 1e6 / static_cast<double>(sampleRate_);
      const double gapUs = static_cast<double>(now - prev) / 1000.0;
      if (gapUs > durationUs * 3.0)
        xruns_.fetch_add(1, std::memory_order_relaxed);
    }
    lastProcessTimeNs_.store(now, std::memory_order_relaxed);
  }

  const uint64_t position = lastPosition_.load(std::memory_order_relaxed);

  if (!planar) {
    float* out = static_cast<float*>(sbuf->datas[0].data);
    renderer_->process(out, frames, channels_, position, sampleRate_);
    sbuf->datas[0].chunk->stride = channels_ * bytesPerSample;
    sbuf->datas[0].chunk->size = frames * channels_ * bytesPerSample;
  } else {
    for (uint32_t c = 0; c < channels_; ++c)
      std::memset(scratch_[c], 0, frames * sizeof(float));
    renderer_->process(reinterpret_cast<float*>(scratch_), frames, channels_,
                       position, sampleRate_);
    for (uint32_t c = 0; c < channels_; ++c) {
      float* planeOut = static_cast<float*>(sbuf->datas[c].data);
      const float* src = scratch_[c];
      for (uint32_t i = 0; i < frames; ++i)
        planeOut[i] = src[i * channels_];
      sbuf->datas[c].chunk->stride = bytesPerSample;
      sbuf->datas[c].chunk->size = frames * bytesPerSample;
    }
  }

  lastPosition_.fetch_add(frames, std::memory_order_relaxed);
  framesProcessed_.fetch_add(frames, std::memory_order_relaxed);
  negotiatedBuffer_.store(frames, std::memory_order_relaxed);
  pw_stream_queue_buffer(stream_, buf);
}

void PipeWireBackend::onParamChanged(uint32_t id, const void* param) noexcept {
  if (id != SPA_PARAM_Format || param == nullptr)
    return;
  const struct spa_pod* pod = static_cast<const struct spa_pod*>(param);
  struct spa_audio_info_raw raw{};
  if (spa_format_audio_raw_parse(pod, &raw) < 0)
    return;
  if (raw.rate != 0) {
    negotiatedRate_.store(raw.rate);
    sampleRate_ = raw.rate;
  }
  if (raw.channels != 0) {
    negotiatedChannels_.store(raw.channels);
    channels_ = raw.channels;
  }
}

void PipeWireBackend::onStateChanged(int state, const char* /*error*/) noexcept {
  streamState_.store(state);
  if (static_cast<pw_stream_state>(state) == PW_STREAM_STATE_ERROR) {
    running_.store(false);
    streamWasError_ = true;
  }
  stateReached_.store(true);
}

void PipeWireBackend::setStreamError(const std::string& error) noexcept {
  std::lock_guard<std::mutex> lk(streamMutex_);
  streamError_ = error;
}

std::string PipeWireBackend::streamErrorLocked() const {
  std::lock_guard<std::mutex> lk(streamMutex_);
  return streamError_;
}

void PipeWireBackend::stop() noexcept {
  running_.store(false);
  if (loop_ != nullptr) {
    // 1. Deactivate the stream under the loop lock (stream API requirement).
    pw_thread_loop_lock(loop_);
    if (stream_ != nullptr)
      pw_stream_set_active(stream_, false);
    pw_thread_loop_unlock(loop_);

    // 2. Quiesce the loop thread WITHOUT holding the lock. Stopping while
    //    holding the lock deadlocks; after this returns no audio callback can
    //    be in flight.
    if (loopStarted_) {
      pw_thread_loop_stop(loop_);
      loopStarted_ = false;
    }
  }
  // 3. With the realtime thread quiesced it is safe to tear everything down.
  if (stream_ != nullptr) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (streamHook_ != nullptr) {
    delete streamHook_;
    streamHook_ = nullptr;
  }
  if (streamCtx_ != nullptr) {
    delete static_cast<StreamCtx*>(streamCtx_);
    streamCtx_ = nullptr;
  }
  if (core_ != nullptr) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  if (context_ != nullptr) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }
  if (loop_ != nullptr) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
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
  l.outputMs = l.bufferMs;
  l.inputMs = l.bufferMs;
  l.roundTripMs = l.outputMs + l.inputMs;
  return l;
}

} // namespace misuc::engine