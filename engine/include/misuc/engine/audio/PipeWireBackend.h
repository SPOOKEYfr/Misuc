#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <misuc/engine/audio/AudioBackend.h>

struct spa_hook;
struct pw_registry;
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;

namespace misuc::engine {

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

  // Called by PipeWire callbacks (public so the callback helpers can reach them).
  void processCallback() noexcept;
  void onParamChanged(uint32_t id, const void* param) noexcept;
  void onStateChanged(int state, const char* error) noexcept;
  void setStreamError(const std::string& error) noexcept;
  mutable std::mutex streamMutex_;
  std::condition_variable stateCv_;

private:
  std::string streamErrorLocked() const;

  static constexpr std::size_t kMaxScratchFrames = 8192;
  static constexpr std::size_t kMaxScratchChannels = 8;
  float scratch_[kMaxScratchChannels][kMaxScratchFrames];

  struct pw_thread_loop* loop_ = nullptr;
  struct pw_context* context_ = nullptr;
  struct pw_core* core_ = nullptr;
  struct pw_stream* stream_ = nullptr;
  struct spa_hook* streamHook_ = nullptr;
  void* streamCtx_ = nullptr;
  bool loopStarted_ = false;

  AudioRenderer* renderer_ = nullptr;
  AudioConfig config_;
  bool input_ = false;
  uint32_t channels_ = 2;
  uint32_t sampleRate_ = 48000;

  std::atomic<bool> running_{false};
  std::atomic<bool> stateReached_{false};
  std::atomic<int> streamState_{0};
  std::string streamError_;
  bool streamWasError_ = false;

  std::atomic<uint64_t> framesProcessed_{0};
  std::atomic<uint32_t> xruns_{0};
  std::atomic<uint32_t> negotiatedRate_{0};
  std::atomic<uint32_t> negotiatedChannels_{0};
  std::atomic<uint32_t> negotiatedBuffer_{0};
  std::atomic<int64_t> lastProcessTimeNs_{-1};
  std::atomic<uint64_t> lastPosition_{0};
};

} // namespace misuc::engine