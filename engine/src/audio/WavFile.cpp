#include <misuc/engine/audio/WavFile.h>

#include <sndfile.h>

#include <cstddef>
#include <cstring>

namespace misuc::audio {

bool readAudioFile(const std::string& path, AudioBuffer& out,
                   std::string& err) noexcept {
  err.clear();
  SF_INFO info{};
  SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
  if (!f) {
    err = "cannot open audio file '" + path + "'";
    return false;
  }
  if (info.channels <= 0 || info.frames <= 0 || info.samplerate <= 0) {
    err = "audio file '" + path + "' has an unsupported layout";
    sf_close(f);
    return false;
  }
  const uint64_t total = static_cast<uint64_t>(info.frames) *
                         static_cast<uint64_t>(info.channels);
  std::vector<float> samples(static_cast<size_t>(total));
  const sf_count_t framesRead =
      sf_readf_float(f, samples.data(), static_cast<sf_count_t>(info.frames));
  const int closeErr = sf_close(f);
  if (framesRead != info.frames || closeErr != 0) {
    err = "failed to read audio file '" + path + "' completely";
    return false;
  }
  out.samples = std::move(samples);
  out.channels = static_cast<uint32_t>(info.channels);
  out.frames = static_cast<uint64_t>(framesRead);
  out.sampleRate = static_cast<double>(info.samplerate);
  return true;
}

bool writeWavSamples(const std::string& path, const float* samples,
                     uint64_t frameCount, uint32_t channels, double sampleRate,
                     bool pcm16, std::string& err) noexcept {
  err.clear();
  if (channels == 0 || frameCount == 0 || sampleRate <= 0.0 || samples == nullptr) {
    err = "invalid audio parameters for WAV export";
    return false;
  }
  SF_INFO info{};
  info.channels = static_cast<int>(channels);
  info.samplerate = static_cast<int>(sampleRate);
  info.frames = static_cast<sf_count_t>(frameCount);
  info.format = SF_FORMAT_WAV | (pcm16 ? SF_FORMAT_PCM_16 : SF_FORMAT_FLOAT);
  SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!f) {
    err = "cannot create audio file '" + path + "'";
    return false;
  }
  const sf_count_t framesWritten =
      sf_writef_float(f, samples, static_cast<sf_count_t>(frameCount));
  const int closeErr = sf_close(f);
  if (framesWritten != static_cast<sf_count_t>(frameCount) || closeErr != 0) {
    err = "failed to write audio file '" + path + "'";
    return false;
  }
  return true;
}

} // namespace misuc::audio