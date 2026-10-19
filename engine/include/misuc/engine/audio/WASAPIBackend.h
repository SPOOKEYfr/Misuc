#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <misuc/engine/audio/AudioBackend.h>

namespace misuc::engine {

// Native Windows WASAPI shared-mode backend. Mirrors the PipeWire backend's
// contract: interleaved F32 throughout, an output stream always, a capture
// stream only when an input device is configured. The capture thread writes
// the latest packet into inScratch_ and the render callback hands it to the
// renderer for monitoring/metering — same lock-free handoff as on Linux.
// All COM/WASAPI handles are opaque void* here so this header stays
// platform-neutral; the real types live only in the .cpp.
class WASAPIBackend final : public AudioBackend {
public:
  WASAPIBackend();
  ~WASAPIBackend() override;

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

  void setStreamError(const std::string& error) noexcept;

private:
  void renderLoop() noexcept;
  void captureLoop() noexcept;

  static constexpr std::size_t kMaxScratchFrames = 8192;
  static constexpr std::size_t kMaxScratchChannels = 8;
  float outScratch_[kMaxScratchChannels][kMaxScratchFrames];
  float inScratch_[kMaxScratchChannels][kMaxScratchFrames];

  // Win32 handles + COM pointers (IAudioClient* etc.), owned by the threads.
  void* exitEvent_ = nullptr;     // manual-reset, wakes both threads
  void* outBufferEvent_ = nullptr; // auto-reset render-side event
  void* inBufferEvent_ = nullptr;  // auto-reset capture-side event

  std::thread outThread_;
  std::thread inThread_;
  std::thread::id outThreadId_;

  AudioRenderer* renderer_ = nullptr;
  AudioConfig config_;
  uint32_t channels_ = 2;
  uint32_t inputChannels_ = 2;
  uint32_t sampleRate_ = 48000;

  std::atomic<bool> running_{false};
  std::atomic<bool> stateReachedOut_{false};
  std::atomic<bool> stateReachedIn_{false};
  std::atomic<int> outState_{0};   // 0 pending, 1 streaming, -1 error
  std::atomic<int> inState_{0};
  std::string streamError_;  // guarded by streamMutex_ like the other backend
  bool streamWasError_ = false;

  std::atomic<uint32_t> inCh_{0};
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

  mutable std::mutex streamMutex_;
  std::condition_variable stateCv_;
};

} // namespace misuc::engine
