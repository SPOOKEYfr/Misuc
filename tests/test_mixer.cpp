// Engine-level mixer tests: per-track gain/pan/mute/solo are applied by the
// audio engine's per-track staging (not baked into the score), track meters
// reflect the real post-gain signal, and offline export honors the mixer state.
// All renders run headless by driving AudioEngine::process() directly.

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "test_utils.h"

namespace {

constexpr uint32_t kSr = 48000u;
// 120 bpm => 0.5 s per beat => 24000 frames per beat.
constexpr uint32_t kFpb120 = 24000u;

inline float peakRange(const std::vector<float>& v, size_t from, size_t to) {
  float p = 0.f;
  for (size_t i = from; i < to && i < v.size(); ++i)
    p = std::max(p, std::fabs(v[i]));
  return p;
}

struct Rendered {
  std::unique_ptr<misuc::engine::AudioEngine> engine;
  std::vector<float> l;
  std::vector<float> r;
};

Rendered renderMix(
    const std::vector<std::vector<misuc::dsp::MidiNote>>& byTrack,
    const std::vector<misuc::engine::MixerTrackParam>& params,
    uint32_t frames) {
  Rendered out;
  out.engine = std::make_unique<misuc::engine::AudioEngine>(
      std::make_unique<misuc::engine::PipeWireBackend>());
  out.engine->setGroove(false);
  out.engine->setMetronome(false, 0.0f);
  out.engine->setTempo(120.0);
  std::vector<misuc::dsp::MidiNote> flat;
  for (const auto& t : byTrack)
    for (const auto& n : t)
      flat.push_back(n);
  out.engine->setScore(std::move(flat));
  if (!params.empty())
    out.engine->publishMixer(params);
  out.engine->play(false);
  std::vector<float> buf(frames * 2, 0.f);
  out.engine->process(nullptr, 0, buf.data(), 2, frames, 0, kSr);
  out.l.resize(frames);
  out.r.resize(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    out.l[i] = buf[2 * i];
    out.r[i] = buf[2 * i + 1];
  }
  return out;
}

} // namespace

// Track A (+6 dB) occupies beats 0..2, track B (0 dB) beats 2..4. In one
// render the first window is A alone and the second is B alone, so each
// track's fader is provably independent and applied only to its own signal.
TEST_CASE(mixer_track_gain_changes_only_its_track) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 1}});

  constexpr uint32_t kFrames = 4 * kFpb120; // beats 0..4
  auto r0 = renderMix(byTrack,
                      {misuc::engine::MixerTrackParam{0.f, 0.f, false, false},
                       misuc::engine::MixerTrackParam{0.f, 0.f, false, false}},
                      kFrames);
  auto rp = renderMix(byTrack,
                      {misuc::engine::MixerTrackParam{6.f, 0.f, false, false},
                       misuc::engine::MixerTrackParam{0.f, 0.f, false, false}},
                      kFrames);
  const float a0 = peakRange(r0.l, 8000, 40000);   // track A at 0 dB
  const float b0 = peakRange(r0.l, 56000, 88000);  // track B at 0 dB
  const float ap = peakRange(rp.l, 8000, 40000);   // track A at +6 dB
  const float bp = peakRange(rp.l, 56000, 88000);  // track B untouched
  TEST_ASSERT(a0 > 0.05f && b0 > 0.05f);
  // +6 dB is a ~1.995x linear boost on A only...
  TEST_ASSERT(std::fabs(ap / a0 - misuc::dsp::gainFromDb(6.f)) < 0.15f);
  // ...and B is bit-for-bit unaffected, since A's fader cannot reach it.
  TEST_ASSERT(std::fabs(bp - b0) < 1e-6f);
}

TEST_CASE(mixer_pan_hard_left_and_right) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});

  constexpr uint32_t kFrames = 2 * kFpb120;
  auto rl = renderMix(byTrack, {misuc::engine::MixerTrackParam{0.f, -1.f,
                                                               false, false}},
                      kFrames);
  TEST_ASSERT(peakRange(rl.l, 8000, 40000) > 0.05f);
  TEST_ASSERT(peakRange(rl.r, 8000, 40000) < 1e-5f);

  auto rr = renderMix(byTrack, {misuc::engine::MixerTrackParam{0.f, 1.f, false,
                                                               false}},
                      kFrames);
  TEST_ASSERT(peakRange(rr.r, 8000, 40000) > 0.05f);
  TEST_ASSERT(peakRange(rr.l, 8000, 40000) < 1e-5f);

  // Centre stays equal-power: both channels alive, neither silent.
  auto rc = renderMix(byTrack,
                      {misuc::engine::MixerTrackParam{0.f, 0.f, false, false}},
                      kFrames);
  TEST_ASSERT(peakRange(rc.l, 8000, 40000) > 0.05f);
  TEST_ASSERT(peakRange(rc.r, 8000, 40000) > 0.05f);
}

// Three tracks, one note each on beats 0..2, 2..4, 4..6. Solo on track 1 must
// silence beats 0..2 and 4..6 and keep only the middle window. Multi-solo must
// keep both soloed tracks and silence the rest.
TEST_CASE(mixer_solo_and_multi_solo) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}});
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {4.0, 2.0, misuc::dsp::midiNoteToFreq(67), 1.f, 2}});

  constexpr uint32_t kFrames = 6 * kFpb120;
  const misuc::engine::MixerTrackParam no{0.f, 0.f, false, false};
  auto rs = renderMix(byTrack, {no, no, misuc::engine::MixerTrackParam{
                                             0.f, 0.f, false, true}},
                      kFrames);
  // Solo on track 2 -> only its beat-4..6 window is audible.
  TEST_ASSERT(peakRange(rs.l, 8000, 40000) < 1e-4f);
  TEST_ASSERT(peakRange(rs.l, 56000, 88000) < 1e-4f);
  TEST_ASSERT(peakRange(rs.l, 104000, 136000) > 0.05f);

  auto rm = renderMix(byTrack,
                      {misuc::engine::MixerTrackParam{0.f, 0.f, false, true},
                       no,
                       misuc::engine::MixerTrackParam{0.f, 0.f, false, true}},
                      kFrames);
  // Multi-solo: track 0 and 2 audible, non-soloed track 1 silent.
  TEST_ASSERT(peakRange(rm.l, 8000, 40000) > 0.05f);
  TEST_ASSERT(peakRange(rm.l, 56000, 88000) < 1e-4f);
  TEST_ASSERT(peakRange(rm.l, 104000, 136000) > 0.05f);
}

TEST_CASE(mixer_mute_gates_track_but_keeps_its_meter) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}});

  constexpr uint32_t kFrames = 2 * kFpb120;
  auto rm = renderMix(byTrack,
                      {misuc::engine::MixerTrackParam{0.f, 0.f, false, false},
                       misuc::engine::MixerTrackParam{0.f, 0.f, true, false}},
                      kFrames);
  // Only track 0 reaches the master; track 1's post-gain signal is silent.
  TEST_ASSERT(peakRange(rm.l, 8000, 40000) > 0.05f);
  // Track 1's meter still shows its real post-gain level (meters are gated by
  // mute/solo for the audible sum, not for the level readout).
  TEST_ASSERT(rm.engine->trackMeterPeakL(1) > 0.01f);
  TEST_ASSERT(rm.engine->trackMeterRmsL(1) > 0.f);

  // With both unmuted the master is louder than with one muted.
  auto run = renderMix(byTrack,
                       {misuc::engine::MixerTrackParam{0.f, 0.f, false, false},
                        misuc::engine::MixerTrackParam{0.f, 0.f, false, false}},
                       kFrames);
  TEST_ASSERT(peakRange(run.l, 8000, 40000) > peakRange(rm.l, 8000, 40000));
  // Both tracks meter alive.
  TEST_ASSERT(run.engine->trackMeterPeakL(0) > 0.01f);
  TEST_ASSERT(run.engine->trackMeterPeakL(1) > 0.01f);
}

TEST_CASE(mixer_export_honors_mixer_state) {
  misuc::project::Session s;
  const std::string ta = s.addTrack("Audible");
  const std::string tm = s.addTrack("Muted");
  s.findTrack(ta)->notes.push_back(
      misuc::project::NoteEvent{0, 60, 127, 0.0, 2.0}); // beats 0..2
  s.findTrack(tm)->notes.push_back(
      misuc::project::NoteEvent{0, 64, 127, 2.0, 2.0}); // beats 2..4

  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(s, notes);

  misuc::engine::AudioEngine engine(
      std::make_unique<misuc::engine::PipeWireBackend>());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0);
  engine.setScore(std::move(notes));
  // Track 1 muted, track 0 panned hard left: the export must reproduce both.
  engine.publishMixer(std::vector<misuc::engine::MixerTrackParam>{
      {0.f, -1.f, false, false}, {0.f, 0.f, true, false}});

  const std::string outPath = "/tmp/misuc_mixer_export_t.wav";
  std::string err;
  TEST_ASSERT(engine.renderToWav(outPath, 0.0, 6.0, 2, err));

  misuc::audio::AudioBuffer wav;
  TEST_ASSERT(misuc::audio::readAudioFile(outPath, wav, err));
  TEST_ASSERT(wav.channels == 2);
  TEST_ASSERT(wav.frames == 6 * kFpb120);
  // Beat 0..2: audible track 0, panned hard left -> left only.
  float l0 = 0.f, r0 = 0.f;
  for (size_t i = 8000; i < 40000; ++i) {
    l0 = std::max(l0, std::fabs(wav.samples[i * 2]));
    r0 = std::max(r0, std::fabs(wav.samples[i * 2 + 1]));
  }
  TEST_ASSERT(l0 > 0.05f);
  TEST_ASSERT(r0 < 1e-4f);
  // Beat 2..6: muted track -> silent (both channels).
  float tail = 0.f;
  for (size_t i = 56000; i < 6 * kFpb120; ++i)
    tail = std::max(tail, std::fabs(wav.samples[i * 2]));
  TEST_ASSERT(tail < 1e-4f);
}