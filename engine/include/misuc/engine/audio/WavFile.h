#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace misuc::audio {

// Decoded audio payload shared by the WAV reader, clip publishing, and the
// export path. Samples are float, interleaved by channel.
struct AudioBuffer {
  std::vector<float> samples; // channels * frames, channel-interleaved
  uint32_t channels = 0;
  uint64_t frames = 0; // frames per channel
  double sampleRate = 0.0;

  double durationSeconds() const {
    return (channels > 0 && sampleRate > 0.0 && samples.size() >= channels)
               ? static_cast<double>(frames) / sampleRate
               : 0.0;
  }
};

// Reads any libsndfile-supported audio file (WAV, FLAC, Ogg/Vorbis, AIFF, ...)
// into a float interleaved buffer. Integer formats are normalised to [-1, 1].
// Returns false and fills `err` on failure.
bool readAudioFile(const std::string& path, AudioBuffer& out,
                   std::string& err) noexcept;

// Writes raw float samples to a WAV file. `samples` holds `frameCount *
// channels` interleaved floats in [-1, 1]. `pcm16` selects a 16-bit PCM file
// (lossless, universally readable); otherwise a 32-bit float WAV is written.
// Returns false and fills `err` on failure.
bool writeWavSamples(const std::string& path, const float* samples,
                     uint64_t frameCount, uint32_t channels, double sampleRate,
                     bool pcm16, std::string& err) noexcept;

// Convenience wrapper over writeWavSamples for an AudioBuffer payload.
inline bool writeWav(const std::string& path, const AudioBuffer& in,
                     std::string& err, bool pcm16 = false) noexcept {
  return writeWavSamples(path, in.samples.data(), in.frames, in.channels,
                         in.sampleRate, pcm16, err);
}

} // namespace misuc::audio