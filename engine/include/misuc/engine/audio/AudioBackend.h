#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <misuc/engine/core/ControlStream.h>

namespace misuc::engine {

struct AudioDeviceInfo {
  std::string id;          // stable backend id (node global id)
  std::string name;        // human-readable name
  std::string description; // longer description
  std::string api;         // "pipewire", "alsa", ...
  bool isInput = false;
};

struct AudioConfig {
  std::string deviceId;        // output device node id (empty = system default)
  std::string inputDeviceId;   // input (capture) node id (empty = no capture)
  uint32_t sampleRate = 48000;
  uint32_t bufferSize = 256;   // frames
  uint32_t channels = 2;       // output channels
  uint32_t inputChannels = 2;
  float monitorGainDb = 0.f;   // input monitoring level
};

struct LatencyInfo {
  double outputMs = 0.0;
  double inputMs = 0.0;
  double roundTripMs = 0.0;
  double bufferMs = 0.0;
};

struct BackendStatus {
  bool running = false;
  uint64_t framesProcessed = 0;
  uint32_t xruns = 0;
  uint32_t negotiatedRate = 0;
  uint32_t negotiatedChannels = 0;
  uint32_t negotiatedInputChannels = 0;
  uint32_t negotiatedBuffer = 0; // frames in most recent buffer
  std::string error;
};

// Audio I/O backend abstraction. The engine stays UI- and backend-agnostic.
class AudioBackend {
public:
  virtual ~AudioBackend() = default;

  virtual std::vector<AudioDeviceInfo> enumerateDevices(bool input) = 0;
  virtual std::vector<uint32_t> supportedSampleRates() const = 0;
  virtual std::vector<uint32_t> supportedBufferSizes() const = 0;

  // Open, then start streaming. `renderer` must outlive `stop()`.
  virtual bool open(const AudioConfig& config, std::string& error) = 0;
  virtual bool start(AudioRenderer& renderer) = 0;
  virtual void stop() noexcept = 0;
  virtual bool isRunning() const = 0;

  virtual BackendStatus status() const = 0;
  virtual LatencyInfo latency() const = 0;
  virtual std::string backendName() const = 0;

  virtual const AudioConfig& config() const = 0;
};

} // namespace misuc::engine