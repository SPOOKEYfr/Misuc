#include <misuc/engine/audio/WavFile.h>

#include <sndfile.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace misuc::audio {

// Fixed-point source bit width for libsndfile writes chosen by the export
// format (FLAC writes PCM16 internally).
static constexpr int kPcmBits = 16;

namespace {

// Triangular (TPDF) dither: two uniform samples summed give a triangle
// distribution spanning ±1 LSB in the source word domain. Deterministic so
// exports are reproducible byte-for-byte.
float tpdfNoise(uint64_t& state) noexcept {
  auto next = [&state]() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>((state >> 33) & 0x7fffffff) / 2147483648.0 -
           0.5;
  };
  return static_cast<float>(next() + next());
}

} // namespace

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
  char buf[4096];
  out.title.clear();
  out.artist.clear();
  if (const char* s = sf_get_string(f, SF_STR_TITLE); s != nullptr) {
    std::strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    out.title = buf;
  }
  if (const char* s = sf_get_string(f, SF_STR_ARTIST); s != nullptr) {
    std::strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    out.artist = buf;
  }
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
  return writeAudioFile(path, samples, frameCount, channels, sampleRate,
                        pcm16 ? ExportFormat::WavPcm16 : ExportFormat::WavFloat,
                        /*dither=*/false, err);
}

bool writeAudioFile(const std::string& path, const float* samples,
                    uint64_t frameCount, uint32_t channels, double sampleRate,
                    ExportFormat format, bool dither, std::string& err,
                    const std::string& title, const std::string& artist) noexcept {
  err.clear();
  if (channels == 0 || frameCount == 0 || sampleRate <= 0.0 ||
      samples == nullptr) {
    err = "invalid audio parameters for export";
    return false;
  }
  SF_INFO info{};
  info.channels = static_cast<int>(channels);
  info.samplerate = static_cast<int>(sampleRate);
  info.frames = static_cast<sf_count_t>(frameCount);
  switch (format) {
    case ExportFormat::WavFloat:
      info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
      break;
    case ExportFormat::WavPcm16:
      info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
      break;
    case ExportFormat::Flac:
      info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
      break;
    case ExportFormat::Mp3:
      info.format = SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
      break;
    case ExportFormat::OggVorbis:
      info.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
      break;
    case ExportFormat::Aiff:
      info.format = SF_FORMAT_AIFF | SF_FORMAT_PCM_16;
      break;
  }

  // Fixed-word-length targets quantise in the float domain so dither can be
  // folded in BEFORE the word is truncated: write a __scale+noise float that
  // libsndfile converts to the (already decorated) PCM word. Lossy codecs
  // (MP3/OGG) get the raw float mix — libsndfile handles encoding.
  const bool pcmOutput = format == ExportFormat::WavPcm16 ||
                         format == ExportFormat::Flac ||
                         format == ExportFormat::Aiff;
  std::vector<float> dithered;
  const float* out = samples;
  if (pcmOutput) {
    dithered.resize(static_cast<size_t>(frameCount) * channels);
    const float scale = static_cast<float>(1u << (kPcmBits - 1)); // 32768
    uint64_t state = static_cast<uint64_t>(frameCount) * 0x9E3779B97F4A7C15ULL +
                     static_cast<uint64_t>(channels) * 2654435761ULL + 1;
    for (size_t i = 0; i < dithered.size(); ++i) {
      float v = samples[i] * scale;
      if (dither)
        v += tpdfNoise(state); // ±1 LSB in the PCM word domain
      v = std::clamp(v, -scale, scale - 1.0f);
      dithered[i] = v / scale;
    }
    out = dithered.data();
  }

  SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!f) {
    err = "cannot create audio file '" + path + "'";
    return false;
  }
  // RIFF INFO (WAV) / Vorbis comment (FLAC) tags from File > Export metadata.
  if (!title.empty())
    sf_set_string(f, SF_STR_TITLE, title.c_str());
  if (!artist.empty())
    sf_set_string(f, SF_STR_ARTIST, artist.c_str());
  const sf_count_t framesWritten =
      sf_writef_float(f, out, static_cast<sf_count_t>(frameCount));
  const int closeErr = sf_close(f);
  if (framesWritten != static_cast<sf_count_t>(frameCount) || closeErr != 0) {
    err = "failed to write audio file '" + path + "'";
    return false;
  }
  return true;
}

std::vector<ExportFormat> availableExportFormats() noexcept {
  // WAV is always available (the oldest, most universal containers).
  std::vector<ExportFormat> out = {ExportFormat::WavFloat, ExportFormat::WavPcm16};
  SF_INFO info{};
  info.channels = 2;
  info.samplerate = 48000;
  auto supported = [&info](int fmt) {
    info.format = fmt;
    // sf_format_check verifies header/encoding combos without touching disk.
    return sf_format_check(&info) != 0;
  };
  if (supported(SF_FORMAT_FLAC | SF_FORMAT_PCM_16))
    out.push_back(ExportFormat::Flac);
  if (supported(SF_FORMAT_OGG | SF_FORMAT_VORBIS))
    out.push_back(ExportFormat::OggVorbis);
  if (supported(SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III))
    out.push_back(ExportFormat::Mp3);
  if (supported(SF_FORMAT_AIFF | SF_FORMAT_PCM_16))
    out.push_back(ExportFormat::Aiff);
  return out;
}

const char* exportFormatSuffix(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::WavFloat:
    case ExportFormat::WavPcm16:
      return "wav";
    case ExportFormat::Flac:
      return "flac";
    case ExportFormat::Mp3:
      return "mp3";
    case ExportFormat::OggVorbis:
      return "ogg";
    case ExportFormat::Aiff:
      return "aiff";
  }
  return "wav";
}

} // namespace misuc::audio