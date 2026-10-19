#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <misuc/engine/audio/AudioBackend.h>

// Forward declarations matching the PipeWire/SPA C types (in namespace ::).
struct spa_hook;
struct pw_registry;
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_properties;
struct spa_pod;
struct spa_pod_builder;

namespace misuc::engine {

// Native PipeWire backend. Runs an output (playback) stream and - when an
// input device is configured - a capture stream on the same thread loop.
// Interleaved F32 throughout; capture arrives on the realtime thread and is
// handed to the renderer for monitofring/metering.
class PipeWireBackend final : public AudioBackend {
public:
  PipeWireBackend();
  ~PipeWireBackend() override;

  std::vector<AudioDeviceInfo> enumerateDevices(bool input) override;
  std::vector<uint32_t> supportedSampleRates() const override;
  std::vector<uint32_t> supportedBufferSizes() const override;

  bool open(const AudioConfig& config, std::string& error) override;
  bool start(AudioRenderer& renderer) override;
  void stop() noexcept override;
  bool isRunning() const override;

  BackendStatus status() const override;
  LatencyInfo latency() const override;
  std::string backendName() const override;

  const AudioConfig& config() const override;

  // Called by PipeWire callbacks (public so the callback helpers can reach them).
  void onOutputProcess() noexcept;
  void onInputProcess() noexcept;
  void onParamChanged(bool input, uint32_t id, const void* param) noexcept;
  void onStateChanged(bool input, int state, const char* error) noexcept;
  void setStreamError(const std::string& error) noexcept;
  mutable std::mutex streamMutex_;
  std::condition_variable stateCv_;

private:
  std::string streamErrorLocked() const;
  bool createStreams(struct pw_properties* props, const struct spa_pod* const* params);

  struct StreamSlot {
    struct pw_stream* stream = nullptr;
    struct spa_hook* hook = nullptr;
    void* ctx = nullptr; // StreamCtx*
    uint32_t channels = 0;
    bool input = false;
  };

  static constexpr std::size_t kMaxScratchFrames = 8192;
  static constexpr std::size_t kMaxScratchChannels = 8;
  float outScratch_[kMaxScratchChannels][kMaxScratchFrames];
  float inScratch_[kMaxScratchChannels][kMaxScratchFrames];

  struct pw_thread_loop* loop_ = nullptr;
  struct pw_context* context_ = nullptr;
  struct pw_core* core_ = nullptr;
  bool loopStarted_ = false;

  StreamSlot outSlot_;
  StreamSlot inSlot_;

  AudioRenderer* renderer_ = nullptr;
  AudioConfig config_;
  uint32_t channels_ = 2;
  uint32_t inputChannels_ = 2;
  uint32_t sampleRate_ = 48000;

  std::atomic<bool> running_{false};
  std::atomic<bool> stateReachedOut_{false};
  std::atomic<bool> stateReachedIn_{false};
  std::atomic<int> outState_{0};
  std::atomic<int> inState_{0};
  std::string streamError_;
  bool streamWasError_ = false;

  std::atomic<uint64_t> framesProcessed_{0};
  std::atomic<uint32_t> xruns_{0};
  std::atomic<uint32_t> negotiatedRate_{0};
  std::atomic<uint32_t> negotiatedChannels_{0};
  std::atomic<uint32_t> negotiatedInputChannels_{0};
  std::atomic<uint32_t> negotiatedBuffer_{0};
  std::atomic<int64_t> lastProcessTimeNs_{-1};
  std::atomic<uint64_t> lastPosition_{0};
  std::atomic<double> outDelayMs_{0.0};
  std::atomic<double> inDelayMs_{0.0};
};

} // namespace misuc::engine