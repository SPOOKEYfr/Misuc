#pragma once

#include <cstdint>

namespace misuc::engine {

// Control messages pushed from the UI/control thread into the real-time audio
// thread. POD by design: the real-time thread never allocates or blocks.
enum class ControlKind : uint32_t {
  Transport   = 1, // transportAction
  SetTempo    = 2, // d0 = BPM
  Metronome   = 3, // f0 = enabled, f1 = countInBeats
  TestTone    = 4, // f0 = enabled(0/1) [device test tone]
  MasterGain  = 5, // f0 = linear gain
  Groove      = 6, // f0 = enabled(0/1)
  MonitorGain = 7, // f0 = linear gain applied to input monitoring
  PreviewNote = 8, // d0 = freq, f0 = velocity gain (audition a note now)
};

struct alignas(8) ControlMessage {
  uint32_t type = 0;
  float f0 = 0.f;
  float f1 = 0.f;
  float f2 = 0.f;
  double d0 = 0.0;
  double d1 = 0.0;
};

enum class TransportAction : uint8_t { Play = 1, Stop = 2, Pause = 3 };

// Audio renderer contract implemented by the engine and called from the
// backend's audio thread.
class AudioRenderer {
public:
  virtual ~AudioRenderer() = default;
  // `output` is an F32 interleaved buffer (outputChannels interleaved) of
  // `frames` frames per channel; the realtime thread may fill it in place.
  // `input` may be null (no capture stream). Must not allocate or block.
  virtual void process(const float* input, uint32_t inputChannels,
                       float* output, uint32_t outputChannels,
                       uint32_t frames, uint64_t framePosition,
                       double sampleRate) noexcept = 0;
};

} // namespace misuc::engine