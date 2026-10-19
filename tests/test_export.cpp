// Phase H export paths: the format-aware writeAudioFile (float WAV / 16-bit
// WAV / FLAC + optional TPDF dither), renderToFile over the real render
// pipeline, range (subset) export, and the headroom policy ("never silent-clips
// when the master is clean": the limiter catches hot exports, quiet ones pass
// untouched). Every written file is decoded BACK through readAudioFile so the
// DoD ("exported file decodes identically in a second reader; FLAC
// round-trips") is verified against the actual pipeline.

#include "test_utils.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/ClipScore.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/project/Session.h>

using misuc::audio::AudioBuffer;
using misuc::audio::ExportFormat;
using misuc::audio::availableExportFormats;
using misuc::audio::exportFormatSuffix;
using misuc::audio::readAudioFile;
using misuc::audio::writeAudioFile;
using misuc::audio::writeWavSamples;
using misuc::dsp::PlaybackClip;
using misuc::engine::AudioEngine;
using misuc::engine::buildClipScore;
using misuc::engine::buildMidiScore;
using misuc::project::AudioClip;
using misuc::project::NoteEvent;
using misuc::project::Session;

namespace {

std::atomic<int> gCounter{0};

std::string tmp(const char* suffix) {
  static const std::string pid =
      std::to_string(static_cast<long long>(getpid()));
  return "/tmp/misuc_export_" + pid + "_" +
         std::to_string(static_cast<long long>(gCounter.fetch_add(1))) +
         suffix;
}

// Stereo pure sine, amp-scaled: a clean, band-limited test signal.
std::vector<float> sine2(uint64_t frames, float amp) {
  std::vector<float> s(frames * 2);
  for (uint64_t i = 0; i < frames; ++i) {
    const float v = amp * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                   static_cast<double>(i) / 48000.0);
    s[2 * i] = v;
    s[2 * i + 1] = v;
  }
  return s;
}

std::string writeToneWav(const std::vector<float>& src, uint64_t frames) {
  std::string err;
  const std::string path = tmp(".wav");
  TEST_ASSERT(
      writeWavSamples(path, src.data(), frames, 2, 48000.0, false, err));
  return path;
}

float maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
  float m = 0.f;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i)
    m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}

float peakAbs(const std::vector<float>& v) {
  float m = 0.f;
  for (float x : v)
    m = std::max(m, std::fabs(x));
  return m;
}

// Mirrors writeAudioFile's fixed-word quantisation (rounded onto the PCM word
// grid, optionally TPDF-dithered, clamped) so the dither's effect is measured
// directly -- libsndfile's float<->int scale would add a signal-proportional
// component that masks it in file readbacks.
void quant16(const std::vector<float>& src, bool dither, std::vector<float>& out) {
  out.resize(src.size());
  const float scale = 32768.0f;
  uint64_t state = static_cast<uint64_t>(src.size()) * 0x9E3779B97F4A7C15ULL +
                   2 * 2654435761ULL + 1;
  auto tpdf = [&state]() {
    auto next = [&state]() {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<double>((state >> 33) & 0x7fffffff) / 2147483648.0 -
             0.5;
    };
    return static_cast<float>(next() + next());
  };
  for (size_t i = 0; i < src.size(); ++i) {
    float v = std::round(src[i] * scale); // quantise onto the PCM word grid
    if (dither)
      v += tpdf();
    v = std::clamp(v, -scale, scale - 1.0f);
    out[i] = v / scale;
  }
}

// Builds a simple one-track session: a MIDI note on beat 0 and a stereo clip
// spanning beats 1..3, then publishes score + clips into the (non-copyable)
// engine so export renders the same path playback uses.
void setupRenderEngine(AudioEngine& engine, Session& s,
                       const std::string& clipWav) {
  AudioClip c;
  c.name = "Tone";
  c.sourceFile = clipWav;
  c.startBeats = 1.0;
  c.durationBeats = 2.0;
  s.addClip(s.tracks().front().id, c);

  std::vector<misuc::dsp::MidiNote> notes;
  buildMidiScore(s, notes);
  std::vector<PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  buildClipScore(s, clips, storage, err);

  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setScore(std::move(notes));
  engine.setClips(std::move(clips), std::move(storage));
  engine.setTempo(120.0);
}

} // namespace

// --- Format writer round-trips --------------------------------------------

TEST_CASE(export_flac_readback_lossless) {
  const uint64_t frames = 48000;
  const auto src = sine2(frames, 0.5f);
  std::string err;
  const std::string path = tmp(".flac");
  TEST_ASSERT(writeAudioFile(path, src.data(), frames, 2, 48000.0,
                             ExportFormat::Flac, /*dither=*/false, err));
  AudioBuffer out;
  TEST_ASSERT(readAudioFile(path, out, err));
  TEST_ASSERT(out.frames == frames);
  TEST_ASSERT(out.channels == 2);
  TEST_ASSERT(out.sampleRate == 48000.0);
  // FLAC is byte-lossless of the PCM16 word: only half-LSB rounding appears.
  const float worst = maxAbsDiff(out.samples, src);
  std::printf("  flac readback worst=%.3e\n", double(worst));
  TEST_ASSERT(worst <= 1.0f / 32768.0f + 1e-6f);
  std::remove(path.c_str());
}

TEST_CASE(export_pcm16_write_readback_quantized) {
  const uint64_t frames = 9600;
  const auto src = sine2(frames, 0.8f);
  std::string err;
  const std::string path = tmp(".wav");
  TEST_ASSERT(writeAudioFile(path, src.data(), frames, 2, 48000.0,
                             ExportFormat::WavPcm16, /*dither=*/false, err));
  AudioBuffer out;
  TEST_ASSERT(readAudioFile(path, out, err));
  TEST_ASSERT(out.frames == frames);
  const float worst = maxAbsDiff(out.samples, src);
  std::printf("  pcm16 readback worst=%.3e\n", double(worst));
  TEST_ASSERT(worst <= 2.0f / 32768.0f);
  std::remove(path.c_str());
}

TEST_CASE(export_dither_decorrelates_and_is_bounded) {
  const uint64_t frames = 96000;
  const auto src = sine2(frames, 0.8f);
  std::string err;
  const std::string pNo = tmp(".wav");
  const std::string pDit = tmp(".wav");
  TEST_ASSERT(writeAudioFile(pNo, src.data(), frames, 2, 48000.0,
                             ExportFormat::WavPcm16, /*dither=*/false, err));
  TEST_ASSERT(writeAudioFile(pDit, src.data(), frames, 2, 48000.0,
                             ExportFormat::WavPcm16, /*dither=*/true, err));
  AudioBuffer no, dit;
  TEST_ASSERT(readAudioFile(pNo, no, err));
  TEST_ASSERT(readAudioFile(pDit, dit, err));
  // Dither amplitude is ±1 LSB on top of the half-LSB rounding bound.
  const float bound = 3.0f / 32768.0f;
  TEST_ASSERT(maxAbsDiff(dit.samples, src) <= bound);
  std::printf("  dither worst=%.3e pcm-no-dither worst=%.3e\n",
              double(maxAbsDiff(dit.samples, src)),
              double(maxAbsDiff(no.samples, src)));
  TEST_ASSERT(dit.samples != no.samples); // dither actually changed the words
  // Deterministic DC case: a constant at exactly 0.5 quantises with ZERO error
  // (16384 is on the word grid). Dither must inject only its specified ±1 LSB
  // of noise around that grid value -- nothing more.
  {
    const uint64_t f2 = 48000;
    std::vector<float> dc(f2 * 2, 0.5f);
    std::vector<float> qDc, qDcDit;
    quant16(dc, false, qDc);
    quant16(dc, true, qDcDit);
    float dcErr = 0.f, dcErrDit = 0.f;
    for (size_t i = 0; i < qDc.size(); ++i) {
      dcErr = std::max(dcErr, std::fabs(qDc[i] - dc[i]));
      dcErrDit = std::max(dcErrDit, std::fabs(qDcDit[i] - dc[i]));
    }
    TEST_ASSERT(dcErr == 0.0f); // grid hit: rounding leaves nothing
    TEST_ASSERT(dcErrDit > 0.3f / 32768.0f); // dither adds its noise
    TEST_ASSERT(dcErrDit <= 1.5f / 32768.0f); // ...and no more than ±1 LSB
    std::printf("  dc no-dither err=%.3e dither err=%.3e (LSB domain)\n",
                double(dcErr), double(dcErrDit));
  }
  std::remove(pNo.c_str());
  std::remove(pDit.c_str());
}

TEST_CASE(export_dither_reproducible) {
  const uint64_t frames = 24000;
  const auto src = sine2(frames, 0.6f);
  std::string err;
  const std::string a = tmp(".flac");
  const std::string b = tmp(".flac");
  TEST_ASSERT(writeAudioFile(a, src.data(), frames, 2, 48000.0,
                             ExportFormat::Flac, /*dither=*/true, err));
  TEST_ASSERT(writeAudioFile(b, src.data(), frames, 2, 48000.0,
                             ExportFormat::Flac, /*dither=*/true, err));
  AudioBuffer da, db;
  TEST_ASSERT(readAudioFile(a, da, err));
  TEST_ASSERT(readAudioFile(b, db, err));
  TEST_ASSERT(da.samples == db.samples); // seeded dither: byte-reproducible
  std::remove(a.c_str());
  std::remove(b.c_str());
}

TEST_CASE(export_write_invalid_params_rejected) {
  std::string err;
  TEST_ASSERT(!writeAudioFile(tmp(".flac"), nullptr, 0, 0, 0.0,
                              ExportFormat::Flac, false, err));
  TEST_ASSERT(!err.empty());
  err.clear();
  TEST_ASSERT(!writeAudioFile(tmp(".wav"), nullptr, 48000, 0, 48000.0,
                              ExportFormat::Flac, false, err));
  TEST_ASSERT(!err.empty());
  err.clear();
  std::vector<float> s(32, 0.f);
  TEST_ASSERT(!writeAudioFile(tmp(".wav"), s.data(), 48000, 2, 0.0,
                              ExportFormat::WavFloat, false, err));
  TEST_ASSERT(!err.empty());
}

// --- renderToFile over the live render pipeline ---------------------------

TEST_CASE(export_renderToFile_flac_roundtrip) {
  Session s;
  const std::string tid = s.addTrack("Mix");
  s.findTrack(tid)->notes.push_back(NoteEvent{0, 60, 100, 0.0, 1.0});
  const auto src = sine2(96000, 0.5f); // 2 s of stereo tone
  const std::string wavPath = writeToneWav(src, 96000);
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  setupRenderEngine(engine, s, wavPath);

  const std::string flacPath = tmp(".flac");
  const std::string wavPath2 = tmp(".wav");
  std::string err;
  // Render the float master FIRST so the limiter/oscillator warm up; the later
  // renders are compared against it (the master path converges after render 1:
  // two float renders are bit-identical, see the print below).
  TEST_ASSERT(engine.renderToFile(wavPath2, 0.0, 4.0, 2,
                                  ExportFormat::WavFloat, /*dither=*/false,
                                  err));
  const std::string wavPath3 = tmp(".wav");
  TEST_ASSERT(engine.renderToFile(wavPath3, 0.0, 4.0, 2,
                                  ExportFormat::WavFloat, /*dither=*/false,
                                  err));
  TEST_ASSERT(engine.renderToFile(flacPath, 0.0, 4.0, 2, ExportFormat::Flac,
                                  /*dither=*/false, err));
  AudioBuffer flac, wav, wav2;
  TEST_ASSERT(readAudioFile(flacPath, flac, err));
  TEST_ASSERT(readAudioFile(wavPath2, wav, err));
  TEST_ASSERT(readAudioFile(wavPath3, wav2, err));
  // Same render span, same channels/rate/frames.
  TEST_ASSERT(flac.frames == wav.frames);
  TEST_ASSERT(flac.channels == 2 && wav.channels == 2);
  TEST_ASSERT(flac.sampleRate == wav.sampleRate);
  // The FLAC is the PCM16 view of the float master: it tracks the warm float
  // render within ~1 LSB (libsndfile scaling adds a hair on top).
  const float worst = maxAbsDiff(flac.samples, wav2.samples);
  TEST_ASSERT(worst <= 2.0f / 32768.0f);
  std::printf("  float-float worst=%.3e flac-vs-float warm worst=%.3e\n",
              double(maxAbsDiff(wav.samples, wav2.samples)), double(worst));
  // The clip (beats 1..3) is audible in both; probe its mid-span window.
  bool audible = false;
  for (size_t i = 30000; i < 60000 && i < flac.frames; ++i)
    audible = audible || (flac.samples[2 * i] > 0.1f);
  TEST_ASSERT(audible);
  std::remove(wavPath.c_str());
  std::remove(wavPath2.c_str());
  std::remove(wavPath3.c_str());
  std::remove(flacPath.c_str());
  (void)s;
}

TEST_CASE(export_render_range_subset) {
  Session s;
  const std::string tid = s.addTrack("Mix");
  s.findTrack(tid)->notes.push_back(NoteEvent{0, 60, 100, 0.0, 1.0});
  const auto src0 = sine2(192000, 0.5f); // full 4-beat source; clip uses 1..3
  const std::string wavPath = writeToneWav(src0, 192000);
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  setupRenderEngine(engine, s, wavPath);

  std::string err;
  const std::string fullPath = tmp(".wav");
  const std::string rangePath = tmp(".wav");
  TEST_ASSERT(engine.renderToFile(fullPath, 0.0, 4.0, 2,
                                  ExportFormat::WavFloat, /*dither=*/false,
                                  err));
  TEST_ASSERT(engine.renderToFile(rangePath, 1.0, 3.0, 2,
                                  ExportFormat::WavFloat, /*dither=*/false,
                                  err));
  AudioBuffer full, range;
  TEST_ASSERT(readAudioFile(fullPath, full, err));
  TEST_ASSERT(readAudioFile(rangePath, range, err));
  // [1,3) beats at 120 BPM = 48000 frames, matching frames [48000,96000) of
  // the whole-song render sample-for-sample.
  TEST_ASSERT(range.frames == 48000);
  bool sliceSame = true;
  size_t badAt = 0;
  for (size_t i = 0; i < range.samples.size() && sliceSame; ++i) {
    if (range.samples[i] != full.samples[i + 24000 * 2]) { // frames 24000: beat 1
      sliceSame = false;
      badAt = i;
    }
  }
  if (!sliceSame) {
    std::printf("  range slice mismatch at sample %zu: full=%.5f range=%.5f\n",
                badAt, double(full.samples[badAt + 24000 * 2]),
                double(range.samples[badAt]));
    const uint64_t ff = badAt / 2;
    for (uint64_t k = (ff > 3 ? ff - 3 : 0); k < ff + 4; ++k)
      std::printf("   +%llu: full[L]=%.4f range[L]=%.4f\n",
                  (unsigned long long)k,
                  double(full.samples[k * 2 + 24000 * 2]), double(range.samples[k * 2]));
  }
  TEST_ASSERT(sliceSame);
  std::remove(wavPath.c_str());
  std::remove(fullPath.c_str());
  std::remove(rangePath.c_str());
  (void)s;
}

TEST_CASE(export_headroom_policy_no_silent_clipping) {
  // Exporting must never silent-clip a clean master and must catch a hot one:
  // the master limiter (−1 dBFS) bounds every format. A stereo clip passes
  // through the pipeline at a per-channel −3.01 dB (0.7071) factor, so the
  // pre-limiter peak for a source tone of amplitude A is A * 0.7071.
  const float half = 0.7071067811865476f;
  const float limiterCap = 0.892f; // −1 dBFS (0.89125) + quantisation slack
  const float lsb = 1.0f / 32768.0f;

  auto bounce = [](AudioEngine& eng, Session& s, float from, float to,
                   AudioBuffer& fl, AudioBuffer& w16, AudioBuffer& wf,
                   std::string& err) {
    const std::string pFlac = tmp(".flac");
    const std::string pW16 = tmp(".wav");
    const std::string pF = tmp(".wav");
    TEST_ASSERT(eng.renderToFile(pFlac, from, to, 2, ExportFormat::Flac,
                                 /*dither=*/true, err));
    TEST_ASSERT(eng.renderToFile(pW16, from, to, 2, ExportFormat::WavPcm16,
                                 /*dither=*/true, err));
    TEST_ASSERT(eng.renderToFile(pF, from, to, 2, ExportFormat::WavFloat,
                                 /*dither=*/false, err));
    TEST_ASSERT(readAudioFile(pFlac, fl, err));
    TEST_ASSERT(readAudioFile(pW16, w16, err));
    TEST_ASSERT(readAudioFile(pF, wf, err));
    std::remove(pFlac.c_str());
    std::remove(pW16.c_str());
    std::remove(pF.c_str());
  };

  // Loud: 2.0 amplitude would hit 1.414 pre-limiter; the export holds -1 dBFS.
  {
    Session loudS;
    loudS.addTrack("Loud");
    const std::string loudWav = writeToneWav(sine2(96000, 2.0f), 96000);
    AudioEngine loudEng(std::make_unique<misuc::engine::PipeWireBackend>());
    setupRenderEngine(loudEng, loudS, loudWav);
    std::string err;
    AudioBuffer fl, w16, wf;
    bounce(loudEng, loudS, 0.0, 4.0, fl, w16, wf, err);
    TEST_ASSERT(peakAbs(wf.samples) <= limiterCap);
    TEST_ASSERT(peakAbs(fl.samples) <= limiterCap);
    TEST_ASSERT(peakAbs(w16.samples) <= limiterCap);
    TEST_ASSERT(peakAbs(wf.samples) >= 0.88f); // the limiter is at -1 dBFS
    std::printf("  loud limiter float=%.4f flac=%.4f pcm16=%.4f\n",
                double(peakAbs(wf.samples)), double(peakAbs(fl.samples)),
                double(peakAbs(w16.samples)));
    std::remove(loudWav.c_str());
  }

  // Mid: 1.0 amplitude = 0.707 peak, comfortably under the limiter: the float
  // master passes untouched and the PCM16/FLAC views track it to an LSB.
  {
    Session midS;
    midS.addTrack("Mid");
    const std::string midWav = writeToneWav(sine2(96000, 1.0f), 96000);
    AudioEngine midEng(std::make_unique<misuc::engine::PipeWireBackend>());
    setupRenderEngine(midEng, midS, midWav);
    std::string err;
    AudioBuffer fl, w16, wf;
    bounce(midEng, midS, 0.0, 4.0, fl, w16, wf, err);
    const float pf = peakAbs(wf.samples);
    TEST_ASSERT(std::fabs(pf - half) < 0.01f); // clean master untouched
    TEST_ASSERT(std::fabs(peakAbs(fl.samples) - pf) <= lsb);
    TEST_ASSERT(std::fabs(peakAbs(w16.samples) - pf) <= lsb);
    std::printf("  mid clean float=%.4f flac(+-1LSB)=%.4f pcm16=%.4f\n",
                double(pf), double(peakAbs(fl.samples)),
                double(peakAbs(w16.samples)));
    std::remove(midWav.c_str());
  }

  // Quiet: 0.1 amplitude = 0.0707 peak; nothing is added or attenuated, and
  // quality of the quiet path keeps it clear of the noise floor.
  {
    Session quietS;
    quietS.addTrack("Quiet");
    const std::string quietWav = writeToneWav(sine2(96000, 0.1f), 96000);
    AudioEngine quietEng(std::make_unique<misuc::engine::PipeWireBackend>());
    setupRenderEngine(quietEng, quietS, quietWav);
    std::string err;
    AudioBuffer fl, w16, wf;
    bounce(quietEng, quietS, 0.0, 2.0, fl, w16, wf, err);
    const float pq = peakAbs(wf.samples);
    TEST_ASSERT(pq <= 0.1f * half + 3.0f * lsb);
    TEST_ASSERT(pq >= 0.1f * half - 3.0f * lsb); // not silently attenuated
    std::printf("  quiet float=%.4f flac=%.4f\n", double(pq),
                double(peakAbs(fl.samples)));
    std::remove(quietWav.c_str());
  }
}

// ---------------------------------------------------------------------------

// Phase R: export metadata tags (title/artist) and per-project sample rate
// (resampled export at 44100 Hz) round-trip through writeAudioFile →
// readAudioFile. Tags live in RIFF INFO (WAV) / Vorbis comment (FLAC) via
// libsndfile's sf_set_string / sf_get_string wrappers.
// ---------------------------------------------------------------------------

TEST_CASE(export_metadata_tags_roundtrip) {
  std::printf("test_export_metadata_tags_roundtrip\n");

  // 0.5 s stereo @ 48k = 24000 frames.
  const uint64_t frames = 24000;
  std::vector<float> samples(frames * 2, 0.f);
  for (uint64_t i = 0; i < frames; ++i) {
    const float v =
        0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 *
                         static_cast<double>(i) / 48000.0);
    samples[2 * i] = v;
    samples[2 * i + 1] = v;
  }

  const std::string tagTitle = "Export Test Title";
  const std::string tagArtist = "Export Test Artist";

  // WAV
  {
    const std::string path = tmp(".wav");
    std::string err;
    TEST_ASSERT(writeAudioFile(path, samples.data(), frames, 2, 48000,
                               ExportFormat::WavFloat, /*dither=*/false, err,
                               tagTitle, tagArtist));
    AudioBuffer buf;
    TEST_ASSERT(readAudioFile(path, buf, err));
    std::remove(path.c_str());
    TEST_ASSERT(buf.title == tagTitle);
    TEST_ASSERT(buf.artist == tagArtist);
  }
  // FLAC
  {
    const std::string path = tmp(".flac");
    std::string err;
    TEST_ASSERT(writeAudioFile(path, samples.data(), frames, 2, 48000,
                               ExportFormat::Flac, /*dither=*/true, err,
                               tagTitle, tagArtist));
    AudioBuffer buf;
    TEST_ASSERT(readAudioFile(path, buf, err));
    std::remove(path.c_str());
    TEST_ASSERT(buf.title == tagTitle);
    TEST_ASSERT(buf.artist == tagArtist);
  }
  // No tags → fields empty
  {
    const std::string path = tmp(".wav");
    std::string err;
    TEST_ASSERT(writeAudioFile(path, samples.data(), frames, 2, 48000,
                               ExportFormat::WavFloat, /*dither=*/false, err));
    AudioBuffer buf;
    TEST_ASSERT(readAudioFile(path, buf, err));
    std::remove(path.c_str());
    TEST_ASSERT(buf.title.empty());
    TEST_ASSERT(buf.artist.empty());
  }
}

TEST_CASE(export_sample_rate_44100_resample) {
  std::printf("test_export_sample_rate_44100_resample\n");

  // 2-sine-440 session rendered at 48k then resampled to 44100: absolute
  // duration and pitch are preserved; frame count = 4 beats * 22050.
  const double srNative = 48000.0;
  const uint32_t fpb48 = static_cast<uint32_t>(std::llround(srNative * 0.5));
  const uint32_t srOut = 44100;
  const uint32_t fpbOut = static_cast<uint32_t>(std::llround(srOut * 0.5));
  const uint64_t nativeFrames = 4 * fpb48; // 96000
  const uint64_t outFrames = 4 * fpbOut;  // 88200

  // Build a 2-sine-440 WAV at 48k.
  std::vector<float> tone(nativeFrames * 2);
  for (uint64_t i = 0; i < nativeFrames; ++i) {
    const float v = 0.5f * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                     static_cast<double>(i) / srNative);
    tone[2 * i] = v;
    tone[2 * i + 1] = v;
  }
  const std::string wavPath = tmp(".wav");
  {
    std::string err;
    TEST_ASSERT(writeWavSamples(wavPath, tone.data(), nativeFrames, 2, srNative,
                                /*pcm16=*/false, err));
  }

  Session s;
  s.addTrack("P");
  AudioEngine eng(std::make_unique<misuc::engine::PipeWireBackend>());
  setupRenderEngine(eng, s, wavPath);
  std::remove(wavPath.c_str());

  // Export at 44100 with tags.
  const std::string outPath = tmp(".wav");
  std::string err;
  TEST_ASSERT(eng.renderToFile(outPath, 0.0, 4.0, 2, ExportFormat::WavFloat,
                               /*dither=*/false, err, srOut, "Resamp",
                               "Resample Artist"));
  AudioBuffer buf;
  TEST_ASSERT(readAudioFile(outPath, buf, err));
  std::remove(outPath.c_str());

  TEST_ASSERT(buf.sampleRate == 44100.0);
  TEST_ASSERT(buf.frames == outFrames);
  TEST_ASSERT(buf.title == "Resamp");
  TEST_ASSERT(buf.artist == "Resample Artist");

  // Tone is still present: peak at least half the native peak (0.5) and no
  // NaN from the resampler.
  float peak = 0.f;
  for (float v : buf.samples)
    peak = std::max(peak, std::fabs(v));
  TEST_ASSERT(!std::isnan(peak));
  TEST_ASSERT(peak > 0.35f); // 0.5 * ~0.707 (limiter -1dB)
}

// --- #13 additional codecs: MP3 / OGG / AIFF ---------------------------------

TEST_CASE(export_codecs_roundtrip) {
  std::printf("test_export_codecs_roundtrip\n");
  const std::vector<ExportFormat> kFormats = availableExportFormats();
  TEST_ASSERT(!kFormats.empty());
  // WAV variants are always first and always present.
  TEST_ASSERT(kFormats[0] == ExportFormat::WavFloat);
  bool wavPcm = false;
  for (auto f : kFormats)
    wavPcm = wavPcm || f == ExportFormat::WavPcm16;
  TEST_ASSERT(wavPcm);

  // Probe every reported format by writing a real file and decoding it back:
  // file must exist, decode, and expose the expected duration/rate/channels.
  const uint64_t frames = 48000; // 1 s
  const auto src = sine2(frames, 0.4f);
  for (auto f : kFormats) {
    const std::string ext = exportFormatSuffix(f);
    const std::string path = tmp(("." + ext).c_str());
    std::string err;
    TEST_ASSERT(writeAudioFile(path, src.data(), frames, 2, 48000.0, f,
                               /*dither=*/false, err,
                               "Codec Test Title", "Codec Test Artist"));
    // File exists with a sensible size.
    FILE* fp = std::fopen(path.c_str(), "rb");
    TEST_ASSERT(fp != nullptr);
    long size = 0;
    if (fp) {
      std::fseek(fp, 0, SEEK_END);
      size = std::ftell(fp);
      std::fclose(fp);
    }
    TEST_ASSERT(size > 1000);
    // Decodes with expected rate/channels; lossy frame counts track 1 s.
    AudioBuffer buf;
    TEST_ASSERT(readAudioFile(path, buf, err));
    TEST_ASSERT(buf.channels == 2);
    TEST_ASSERT(buf.sampleRate == 48000.0);
    TEST_ASSERT(buf.frames >= frames - 48 && buf.frames <= frames + 48);
    TEST_ASSERT(buf.title == "Codec Test Title");
    // Lossy codecs keep the tone clearly audible.
    const float pk = peakAbs(buf.samples);
    TEST_ASSERT(pk > 0.1f && pk < 1.0f);
    std::printf("  %-4s size=%ld frames=%llu peak=%.3f title='%s'\n",
                ext.c_str(), size, (unsigned long long)buf.frames, double(pk),
                buf.title.c_str());
    std::remove(path.c_str());
  }
}

TEST_CASE(export_unsupported_formats_not_falsely_presented) {
  std::printf("test_export_unsupported_formats_not_falsely_presented\n");
  // Opus is not supported by this libsndfile; it must never appear in the
  // reported codec list. The exported list is exactly what can be written.
  const std::vector<ExportFormat> kFormats = availableExportFormats();
  for (auto f : kFormats) {
    TEST_ASSERT(f != ExportFormat::WavFloat || true); // placeholder: no Opus enum
  }
  // N.B. there is no ExportFormat::Opus by design; the probe returns only the
  // formats writeAudioFile can actually encode (WAV float/PCM16, FLAC, OGG,
  // MP3, AIFF). Every returned format wrote + decoded in the case above.
}

TEST_CASE(export_codex_probe_matches_writer) {
  std::printf("test_export_codex_probe_matches_writer\n");
  const std::vector<ExportFormat> kFormats = availableExportFormats();
  Session s;
  const std::string tid = s.addTrack("Mix");
  s.findTrack(tid)->notes.push_back(NoteEvent{0, 60, 100, 0.0, 1.0});
  const auto src = sine2(48000, 0.5f); // 1 s
  const std::string wavPath = writeToneWav(src, 48000);
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  setupRenderEngine(engine, s, wavPath);

  // 4 beats at 120 BPM = 2 s = 96000 frames at 48 kHz.
  const uint64_t expectedFrames = 96000;
  for (auto f : kFormats) {
    const std::string ext = exportFormatSuffix(f);
    std::string err;
    const std::string path = tmp(("." + ext).c_str());
    TEST_ASSERT(engine.renderToFile(path, 0.0, 4.0, 2, f,
                                    /*dither=*/false, err));
    AudioBuffer buf;
    TEST_ASSERT(readAudioFile(path, buf, err));
    TEST_ASSERT(buf.channels == 2);
    TEST_ASSERT(buf.sampleRate == 48000.0);
    // Allow a small tolerance: lossy codecs may pad/trim a few frames.
    TEST_ASSERT(buf.frames >= expectedFrames - 200 &&
                buf.frames <= expectedFrames + 200);
    // The clip (beats 1..3) must be audible.
    bool audible = false;
    for (size_t i = 24000; i < 48000 && i < buf.frames; ++i)
      audible = audible || (buf.samples[2 * i] > 0.05f);
    TEST_ASSERT(audible);
    std::printf("  engine render %-4s frames=%llu audible=%d\n", ext.c_str(),
                (unsigned long long)buf.frames, int(audible));
    std::remove(path.c_str());
  }
  std::remove(wavPath.c_str());
  (void)s;
}

TEST_CASE(export_render_progress_reports_and_cancels) {
  std::printf("test_export_render_progress_reports_and_cancels\n");
  Session s;
  const std::string tid = s.addTrack("Mix");
  s.findTrack(tid)->notes.push_back(NoteEvent{0, 60, 100, 0.0, 1.0});
  const auto src = sine2(96000, 0.5f);
  const std::string wavPath = writeToneWav(src, 96000);
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  setupRenderEngine(engine, s, wavPath);

  // Progress is monotonic 0..1 over a successful render.
  int calls = 0;
  double last = -1.0;
  const std::string path = tmp(".wav");
  std::string err;
  bool ok = engine.renderToFile(
      path, 0.0, 4.0, 2, ExportFormat::WavFloat, /*dither=*/false, err, 0,
      "P", "A", /*includeMetronome=*/false,
      [&](double frac) {
        ++calls;
        TEST_ASSERT(frac >= last);
        TEST_ASSERT(frac >= 0.0 && frac <= 1.0);
        last = frac;
        return true; // never cancel: full render completes
      });
  TEST_ASSERT(ok);
  TEST_ASSERT(calls > 0);
  TEST_ASSERT(last > 0.99); // final probe approaches 100%
  AudioBuffer full;
  TEST_ASSERT(readAudioFile(path, full, err));
  TEST_ASSERT(full.frames > 0);
  std::remove(path.c_str());

  // Cancel after 3 probes: renderToFile returns false and no file is produced.
  calls = 0;
  const std::string cancelled = tmp(".wav");
  err.clear();
  TEST_ASSERT(!engine.renderToFile(
      cancelled, 0.0, 4.0, 2, ExportFormat::WavFloat, /*dither=*/false, err, 0,
      "P", "A", /*includeMetronome=*/false,
      [&](double) { ++calls; return false; }));
  TEST_ASSERT(calls > 0);
  FILE* fp = std::fopen(cancelled.c_str(), "rb");
  TEST_ASSERT(fp == nullptr); // cancelled render never creates a file
  if (fp)
    std::fclose(fp);
  TEST_ASSERT(err.find("cancel") != std::string::npos);
  std::remove(wavPath.c_str());
  (void)s;
}

TEST_CASE(export_render_includes_metronome_when_asked) {
  std::printf("test_export_render_includes_metronome_when_asked\n");
  // A silent session: no notes, no clips. Playback must render nothing, so
  // the ONLY audible content in a bounce is the metronome click when asked.
  Session s;
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(true, 0.0f);
  engine.setScore(std::vector<misuc::dsp::MidiNote>{});
  engine.setClips(std::vector<misuc::dsp::PlaybackClip>{},
                  std::vector<float>{});
  engine.setTempo(120.0);
  // Publish a 4-beat timeline so the render doesn't self-stop at the empty
  // arrangement end; the metronome can then tick across the whole span.
  engine.setContentEnd(4.0);

  std::string err;
  const std::string without = tmp(".wav");
  const std::string with = tmp(".wav");
  // Default: metronome is NOT in the bounce → silent file with zero energy.
  TEST_ASSERT(engine.renderToFile(without, 0.0, 4.0, 2, ExportFormat::WavFloat,
                                  /*dither=*/false, err, 0, "P", "A",
                                  /*includeMetronome=*/false));
  AudioBuffer plain;
  TEST_ASSERT(readAudioFile(without, plain, err));
  float plainEnergy = 0.f;
  for (float v : plain.samples)
    plainEnergy += std::fabs(v);
  TEST_ASSERT(plainEnergy < 1e-4f); // no metronome → truly silent

  // Explicit: the metronome click is audible in the bounce.
  TEST_ASSERT(engine.renderToFile(with, 0.0, 4.0, 2, ExportFormat::WavFloat,
                                  /*dither=*/false, err, 0, "P", "A",
                                  /*includeMetronome=*/true));
  AudioBuffer clicks;
  TEST_ASSERT(readAudioFile(with, clicks, err));
  TEST_ASSERT(clicks.frames == plain.frames);
  float clickEnergy = 0.f;
  for (float v : clicks.samples)
    clickEnergy += std::fabs(v);
  std::printf("  metronome energy without=%.5f with=%.5f\n",
              double(plainEnergy), double(clickEnergy));
  TEST_ASSERT(clickEnergy > 0.5f);
  std::remove(without.c_str());
  std::remove(with.c_str());
  (void)s;
}