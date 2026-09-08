#include "test_utils.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/ClipScore.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/dsp/ClipPlayer.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

using misuc::audio::AudioBuffer;
using misuc::audio::readAudioFile;
using misuc::audio::writeWavSamples;
using misuc::dsp::ClipPlayer;
using misuc::dsp::PlaybackClip;
using misuc::engine::AudioEngine;
using misuc::engine::buildClipScore;
using misuc::project::AudioClip;
using misuc::project::NoteEvent;
using misuc::project::Session;
using misuc::project::Track;

namespace {

std::atomic<int> g_tempCounter{0};

std::string tempPath(const char* suffix) {
  static std::string pid = std::to_string(static_cast<long long>(getpid()));
  const int n = g_tempCounter.fetch_add(1);
  return "/tmp/misuc_clip_test_" + pid + "_" + std::to_string(n) + suffix;
}

// Writes a mono sine tone WAV and returns its path.
std::string writeToneWav(double freq, double seconds, float amp,
                         double sampleRate) {
  const uint64_t frames = static_cast<uint64_t>(seconds * sampleRate);
  std::vector<float> s(frames);
  for (uint64_t i = 0; i < frames; ++i)
    s[i] = amp * static_cast<float>(std::sin(
                      2.0 * 3.14159265358979 * freq *
                      static_cast<double>(i) / sampleRate));
  std::string err;
  const std::string path = tempPath(".wav");
  TEST_ASSERT(writeWavSamples(path, s.data(), frames, 1, sampleRate,
                              /*pcm16=*/false, err));
  return path;
}

float peak(const std::vector<float>& v, size_t from, size_t to) {
  float m = 0.f;
  for (size_t i = from; i < to && i < v.size(); ++i)
    m = std::max(m, std::fabs(v[i]));
  return m;
}

// Peak over an interleaved buffer by output frame (channel-strided), so
// `from`/`to` are frame ranges like the mono `peak` uses.
float peakByFrame(const std::vector<float>& v, size_t from, size_t to,
                  uint32_t channels) {
  float m = 0.f;
  for (size_t i = from; i < to && i * channels < v.size(); ++i)
    for (uint32_t c = 0; c < channels; ++c)
      m = std::max(m, std::fabs(v[i * channels + c]));
  return m;
}

} // namespace

// --- WAV file I/O ---------------------------------------------------------

TEST_CASE(wav_float_roundtrip) {
  AudioBuffer src;
  src.channels = 1;
  src.frames = 48000;
  src.sampleRate = 48000.0;
  src.samples.resize(48000);
  for (size_t i = 0; i < 48000; ++i)
    src.samples[i] = 0.4f * std::sin(2.0 * 3.14159265358979 * 440.0 *
                                     static_cast<double>(i) / 48000.0);
  std::string err;
  const std::string path = tempPath(".wav");
  TEST_ASSERT(writeWavSamples(path, src.samples.data(), src.frames, 1, 48000,
                              false, err));

  AudioBuffer got;
  TEST_ASSERT(readAudioFile(path, got, err));
  TEST_ASSERT(got.channels == 1);
  TEST_ASSERT(got.frames == 48000);
  TEST_ASSERT(std::fabs(got.sampleRate - 48000.0) < 1.0);
  TEST_ASSERT(got.samples.size() == 48000);
  TEST_ASSERT(std::fabs(got.samples[1000] - src.samples[1000]) < 1e-6f);
}

TEST_CASE(wav_pcm16_roundtrip) {
  const std::string path = tempPath(".wav");
  std::vector<float> s(2400);
  for (size_t i = 0; i < 2400; ++i)
    s[i] = 0.8f * std::sin(2.0 * 3.14159265358979 * 440.0 *
                           static_cast<double>(i) / 48000.0);
  std::string err;
  TEST_ASSERT(writeWavSamples(path, s.data(), 2400, 2, 48000, true, err));
  AudioBuffer got;
  TEST_ASSERT(readAudioFile(path, got, err));
  TEST_ASSERT(got.channels == 2);
  TEST_ASSERT(got.frames == 2400);
  TEST_ASSERT(std::fabs(got.sampleRate - 48000.0) < 1.0);
  // 16-bit quantisation bound.
  TEST_ASSERT(std::fabs(got.samples[100] - s[100]) < 0.001f);
}

TEST_CASE(wav_bad_path_fails) {
  std::string err;
  AudioBuffer got;
  TEST_ASSERT(!readAudioFile("/nonexistent/misuc_never.wav", got, err));
  TEST_ASSERT(!err.empty());
}

// --- Session model --------------------------------------------------------

TEST_CASE(session_clips_roundtrip) {
  Session s;
  const std::string tid = s.addTrack("Audio");
  AudioClip c;
  c.name = "Kick";
  c.sourceFile = "/tmp/kick.wav";
  c.startBeats = 2.5;
  c.durationBeats = 4.0;
  c.clipOffsetBeats = 0.5;
  c.gainDb = -3.0f;
  c.fadeInBeats = 0.25;
  c.fadeOutBeats = 0.75;
  c.sampleRate = 48000;
  c.channels = 2;
  const std::string cid = s.addClip(tid, c);
  TEST_ASSERT(!cid.empty());

  const Session loaded = Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.tracks().size() == 1);
  TEST_ASSERT(loaded.tracks()[0].clips.size() == 1);
  const AudioClip& l = loaded.tracks()[0].clips[0];
  TEST_ASSERT(l.id == cid);
  TEST_ASSERT(l.name == "Kick");
  TEST_ASSERT(l.sourceFile == "/tmp/kick.wav");
  TEST_ASSERT(l.startBeats == 2.5);
  TEST_ASSERT(l.durationBeats == 4.0);
  TEST_ASSERT(l.clipOffsetBeats == 0.5);
  TEST_ASSERT(std::fabs(l.gainDb - (-3.0f)) < 0.001f);
  TEST_ASSERT(l.fadeInBeats == 0.25);
  TEST_ASSERT(l.fadeOutBeats == 0.75);
  TEST_ASSERT(l.sampleRate == 48000);
  TEST_ASSERT(l.channels == 2);
}

TEST_CASE(session_clip_ids_unique_across_load) {
  Session s;
  const std::string tid = s.addTrack("T");
  AudioClip c;
  c.name = "A";
  const std::string a = s.addClip(tid, c);
  const Session loaded = Session::fromJson(s.toJson());
  AudioClip n;
  n.name = "B";
  std::string tid_loaded = loaded.tracks()[0].id;
  // Reloaded track, new clip gets a non-colliding id after re-add on a copy.
  Session copy = loaded;
  const std::string b = copy.addClip(tid_loaded, n);
  TEST_ASSERT(a != b);
  TEST_ASSERT(copy.tracks()[0].clips.size() == 2);
}

// --- Clip score flattening ------------------------------------------------

TEST_CASE(build_clip_score_decodes) {
  const std::string path = writeToneWav(440.0, 1.0, 0.5f, 48000.0);
  Session s;
  const std::string tid = s.addTrack("Audio");
  Track* t = s.findTrack(tid);
  t->gainDb = -6.0f;
  AudioClip c;
  c.name = "Tone";
  c.sourceFile = path;
  c.startBeats = 1.0;
  c.durationBeats = 2.0;
  c.gainDb = 2.0f;
  s.addClip(tid, c);

  std::vector<PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  buildClipScore(s, clips, storage, err);
  TEST_ASSERT(err.empty());
  TEST_ASSERT(clips.size() == 1);
  TEST_ASSERT(storage.size() == 48000); // mono, 1 s at 48k
  const PlaybackClip& pc = clips[0];
  TEST_ASSERT(pc.samples != nullptr);
  TEST_ASSERT(pc.channels == 1);
  TEST_ASSERT(pc.frames == 48000);
  TEST_ASSERT(std::fabs(pc.sampleRate - 48000.0) < 1.0);
  TEST_ASSERT(pc.startBeats == 1.0);
  TEST_ASSERT(pc.durationBeats == 2.0);
  // Only the clip's own gain is baked now; track gain is applied by the mixer.
  TEST_ASSERT(std::fabs(pc.gain - misuc::dsp::gainFromDb(2.0f)) < 1e-4f);
  TEST_ASSERT(pc.track == 0);
  // Samples reference the shared storage buffer and carry the tone amplitude.
  TEST_ASSERT(peak(storage, 1000, 4000) > 0.4f);
}

TEST_CASE(build_clip_score_missing_file_degrades) {
  Session s;
  const std::string tid = s.addTrack("Audio");
  AudioClip c;
  c.name = "Gone";
  c.sourceFile = "/nonexistent/gone.wav";
  c.startBeats = 0.0;
  c.durationBeats = 1.0;
  s.addClip(tid, c);

  std::vector<PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  buildClipScore(s, clips, storage, err);
  TEST_ASSERT(clips.empty());
  TEST_ASSERT(!err.empty());
}

// --- ClipPlayer DSP -------------------------------------------------------

TEST_CASE(clip_player_plays_region) {
  // 1 s mono 440 Hz tone at 48k.
  const std::string path = writeToneWav(440.0, 1.0, 0.5f, 48000.0);
  AudioBuffer buf;
  std::string err;
  TEST_ASSERT(readAudioFile(path, buf, err));
  PlaybackClip c;
  c.samples = buf.samples.data();
  c.channels = 1;
  c.frames = static_cast<uint32_t>(buf.frames);
  c.sampleRate = 48000.0;
  c.startBeats = 2.0;  // 1 s in at 120 bpm
  c.durationBeats = 2.0; // 1 s of timeline
  c.gain = 0.5f;

  ClipPlayer player;
  player.setSampleRate(48000.0);
  player.setTempo(120.0);
  std::vector<PlaybackClip> clips{c};
  constexpr uint32_t kFrames = 96000; // 4 beats
  // One interleaved stereo buffer, the same layout the engine mixes into.
  std::vector<float> out(kFrames * 2, 0.f);
  player.render(clips, out.data(), out.data(), kFrames, 2, 0);

  // Before the clip start (frames 0..48000): silent.
  TEST_ASSERT(peakByFrame(out, 1000, 47000, 2) < 1e-4f);
  // Inside the clip (frames 48000..96000): tone present.
  TEST_ASSERT(peakByFrame(out, 48200, 72000, 2) > 0.01f);
}

TEST_CASE(clip_player_offset_trims_in) {
  const std::string path = writeToneWav(440.0, 1.0, 0.5f, 48000.0);
  AudioBuffer buf;
  std::string err;
  TEST_ASSERT(readAudioFile(path, buf, err));
  PlaybackClip c;
  c.samples = buf.samples.data();
  c.channels = 1;
  c.frames = static_cast<uint32_t>(buf.frames);
  c.sampleRate = 48000.0;
  c.startBeats = 0.0;
  c.offsetBeats = 1.0; // skip the first 0.5 s of source
  c.durationBeats = 2.0;
  c.gain = 0.5f;

  ClipPlayer player;
  player.setSampleRate(48000.0);
  player.setTempo(120.0);
  std::vector<PlaybackClip> clips{c};
  constexpr uint32_t kFrames = 96000;
  std::vector<float> out(kFrames * 2, 0.f);
  player.render(clips, out.data(), out.data(), kFrames, 2, 0);

  // First beat (0..24000): trimmed out by the offset -> silent.
  TEST_ASSERT(peakByFrame(out, 200, 23000, 2) < 1e-4f);
  // Second beat (24000..48000): the tone's second half plays.
  TEST_ASSERT(peakByFrame(out, 24200, 47000, 2) > 0.01f);
}

TEST_CASE(clip_player_fades_endpoints) {
  const std::string path = writeToneWav(440.0, 1.0, 0.5f, 48000.0);
  AudioBuffer buf;
  std::string err;
  TEST_ASSERT(readAudioFile(path, buf, err));
  PlaybackClip c;
  c.samples = buf.samples.data();
  c.channels = 1;
  c.frames = static_cast<uint32_t>(buf.frames);
  c.sampleRate = 48000.0;
  c.startBeats = 0.0;
  c.durationBeats = 2.0;       // 1 s
  c.fadeInBeats = 0.5;         // 0.25 s
  c.fadeOutBeats = 0.5;        // 0.25 s
  c.gain = 0.8f;

  ClipPlayer player;
  player.setSampleRate(48000.0);
  player.setTempo(120.0);
  std::vector<PlaybackClip> clips{c};
  constexpr uint32_t kFrames = 96000;
  std::vector<float> out(kFrames * 2, 0.f);
  player.render(clips, out.data(), out.data(), kFrames, 2, 0);

  float first = peakByFrame(out, 0, 2000, 2);
  float mid = peakByFrame(out, 40000, 44000, 2);
  float last = peakByFrame(out, 46000, 48000, 2);
  TEST_ASSERT(peakByFrame(out, 0, 300, 2) < 0.02f);
  TEST_ASSERT(first > 0.005f); // ramp is already audible shortly after
  TEST_ASSERT(first < mid);
  TEST_ASSERT(mid > 0.05f);
  TEST_ASSERT(last < mid * 0.6f);
}

// --- Engine: record, clip playback, offline render (headless) -------------

TEST_CASE(record_capture_headless) {
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  constexpr uint32_t kChunk = 4800;
  std::vector<float> out(kChunk * 2, 0.f);

  // Play, then arm record. Each process() call drains the control queue.
  engine.play(false);
  engine.process(nullptr, 0, out.data(), 2, 32, 0, 48000);
  engine.startRecord();
  engine.process(nullptr, 0, out.data(), 2, 32, 32, 48000);
  TEST_ASSERT(engine.isRecording());

  // Feed 10 mono chunks of a 0.25-amplitude 1 kHz sine into the engine.
  std::vector<float> input(kChunk);
  for (uint32_t i = 0; i < kChunk; ++i)
    input[i] = 0.25f * std::sin(2.0 * 3.14159265358979 * 1000.0 *
                                static_cast<double>(i) / 48000.0);
  uint64_t pos = 64;
  for (int rep = 0; rep < 10; ++rep) {
    engine.process(input.data(), 1, out.data(), 2, kChunk, pos, 48000);
    pos += kChunk;
  }
  TEST_ASSERT(engine.isRecording());

  engine.stopRecord();
  engine.process(nullptr, 0, out.data(), 2, 32, pos, 48000); // drain stop
  TEST_ASSERT(!engine.isRecording());
  std::vector<float> rec;
  uint32_t sr = 0, ch = 0;
  TEST_ASSERT(engine.takeRecordedSamples(rec, sr, ch));
  TEST_ASSERT(ch == 1);
  TEST_ASSERT(sr == 48000);
  TEST_ASSERT(rec.size() == 10 * kChunk);
  TEST_ASSERT(peak(rec, 2000, rec.size() - 2000) > 0.2f);
}

TEST_CASE(clip_playback_through_engine_headless) {
  // A 1 s 440 Hz tone clip placed at beat 2 (so the first second is silent).
  const std::string path = writeToneWav(440.0, 1.0, 0.5f, 48000.0);
  Session s;
  const std::string tid = s.addTrack("Audio");
  Track* t = s.findTrack(tid);
  AudioClip c;
  c.name = "Tone";
  c.sourceFile = path;
  c.startBeats = 2.0;
  c.durationBeats = 2.0;
  s.addClip(tid, c);

  std::vector<PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  buildClipScore(s, clips, storage, err);
  TEST_ASSERT(clips.size() == 1);

  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setClips(std::move(clips), std::move(storage));
  engine.play(false);

  constexpr uint32_t kFrames = 96000; // 4 beats at 120 bpm
  std::vector<float> out(kFrames * 2, 0.f);
  engine.process(nullptr, 0, out.data(), 2, kFrames, 0, 48000);

  // First two beats (frames 0..48000) are before the clip start.
  float before = 0.f;
  for (size_t i = 4000; i < 40000; ++i)
    before = std::max(before, std::fabs(out[2 * i]));
  TEST_ASSERT(before < 1e-3f);
  // Clip window (frames 48000..96000): the tone should be audible.
  float inside = 0.f;
  for (size_t i = 49000; i < 70000; ++i)
    inside = std::max(inside, std::fabs(out[2 * i]));
  TEST_ASSERT(inside > 0.05f);
}

TEST_CASE(render_to_wav_offline) {
  Session s;
  const std::string tid = s.addTrack("Mix");
  Track* t = s.findTrack(tid);
  // One MIDI note on beat 0 and one audio clip on beats 1..2.
  t->notes.push_back(NoteEvent{0, 60, 100, 0.0, 1.0});
  const std::string path = writeToneWav(330.0, 1.0, 0.5f, 48000.0);
  AudioClip c;
  c.name = "Tone";
  c.sourceFile = path;
  c.startBeats = 1.0;
  c.durationBeats = 2.0;
  s.addClip(tid, c);

  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(s, notes);
  TEST_ASSERT(notes.size() == 1);
  std::vector<PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  buildClipScore(s, clips, storage, err);
  TEST_ASSERT(clips.size() == 1);

  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setScore(std::move(notes));
  engine.setClips(std::move(clips), std::move(storage));
  engine.setTempo(120.0);

  const std::string outPath = tempPath(".wav");
  TEST_ASSERT(engine.renderToWav(outPath, 0.0, 4.0, 2, err));

  AudioBuffer mix;
  TEST_ASSERT(readAudioFile(outPath, mix, err));
  TEST_ASSERT(mix.channels == 2);
  TEST_ASSERT(mix.frames == 96000); // 4 beats at 120 bpm == 2 s
  TEST_ASSERT(mix.sampleRate == 48000.0);

  // The first beat carries the MIDI note (gain 100/127, kLevel-scaled).
  float firstBeat = 0.f;
  for (size_t i = 6000; i < 20000; ++i)
    firstBeat = std::max(firstBeat, std::fabs(mix.samples[2 * i]));
  TEST_ASSERT(firstBeat > 1e-3f);
  // The clip beat carries the tone.
  float clipBeat = 0.f;
  for (size_t i = 25000; i < 46000; ++i)
    clipBeat = std::max(clipBeat, std::fabs(mix.samples[2 * i]));
  TEST_ASSERT(clipBeat > 0.05f);
  // The clip spans beats 1..3; the region after beat 3 must be silent.
  float tail = 0.f;
  for (size_t i = 73000; i < 95000; ++i)
    tail = std::max(tail, std::fabs(mix.samples[2 * i]));
  TEST_ASSERT(tail < 1e-3f);
}

// A published one-shot sample preview must render through the wall-clock path
// even while the transport is stopped (browser audition), with a click-free
// attack. Headless: we drive process() directly, exactly like clipboard
// playback, so no sound daemon is required.
TEST_CASE(preview_sample_headless) {
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0);

  // 0.5 s of 440 Hz mono at amplitude 0.5, 48000 Hz.
  constexpr uint32_t kSr = 48000;
  constexpr uint32_t kFrames = kSr / 2;
  std::vector<float> s;
  s.reserve(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i)
    s.push_back(0.5f * static_cast<float>(
                         std::sin(2.0 * 3.14159265358979 * 440.0 *
                                  static_cast<double>(i) / kSr)));
  engine.previewSample(s, 1);

  std::vector<float> out(kSr * 2, 0.f);
  engine.process(nullptr, 0, out.data(), 2, kSr, 0, kSr);

  // The first frame after the attack edge should already carry the tone.
  float early = 0.f;
  for (size_t i = 512; i < 16000; ++i)
    early = std::max(early, std::fabs(out[2 * i]));
  TEST_ASSERT(early > 0.05f);
  // The one-shot ends when the buffer is exhausted; the tail must be silent.
  float tail = 0.f;
  for (size_t i = 36000; i < 47000; ++i)
    tail = std::max(tail, std::fabs(out[2 * i]));
  TEST_ASSERT(tail < 1e-3f);
}

// Ruler seek: the playhead jumps to the clicked beat while stopped, and Play
// resumes from the caret rather than rewinding to bar 1. Headless via process().
TEST_CASE(seek_transport_headless) {
  AudioEngine engine(std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0); // 24000 frames per beat @48k

  std::vector<float> out(4096 * 2, 0.f);
  engine.process(nullptr, 0, out.data(), 2, 4096, 0, 48000);
  TEST_ASSERT(engine.playheadSamples() == 0);

  // Seek to beat 2 while stopped.
  engine.seekToBeat(2.0);
  engine.process(nullptr, 0, out.data(), 2, 4096, 0, 48000);
  TEST_ASSERT(engine.playheadSamples() == 48000);
  TEST_ASSERT(!engine.isTransportPlaying());

  // Play must continue from the caret, not restart at bar 1.
  engine.play(false);
  engine.process(nullptr, 0, out.data(), 2, 4096, 0, 48000);
  TEST_ASSERT(engine.isTransportPlaying());
  TEST_ASSERT(engine.playheadSamples() > 48000);

  engine.stop();
}