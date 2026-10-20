// Engine-level mixer tests: per-track gain/pan/mute/solo are applied by the
// audio engine's per-track staging (not baked into the score), track meters
// reflect the real post-gain signal, and offline export honors the mixer state.
// Automation ("gain") lanes are proven audible in the same way: the curve is
// sampled against the transport position and interpolated per sample in both
// live process() rendering and renderToWav. All renders run headless by
// driving AudioEngine::process() directly.

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/Json.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <cmath>
#include <map>
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
      misuc::test::defaultAudioBackend());
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

// Same render path as renderMix but with published automation curves instead
// of (or alongside) static mixer params.
Rendered renderAuto(
    const std::vector<std::vector<misuc::dsp::MidiNote>>& byTrack,
    const std::vector<misuc::engine::AutomationParam>& automation,
    uint32_t frames) {
  Rendered out;
  out.engine = std::make_unique<misuc::engine::AudioEngine>(
      misuc::test::defaultAudioBackend());
  out.engine->setGroove(false);
  out.engine->setMetronome(false, 0.0f);
  out.engine->setTempo(120.0);
  std::vector<misuc::dsp::MidiNote> flat;
  for (const auto& t : byTrack)
    for (const auto& n : t)
      flat.push_back(n);
  out.engine->setScore(std::move(flat));
  out.engine->publishAutomation(automation);
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

// Same render path as renderMix but with a paired bus track published alongside
// the mixer tracks, so tests exercise the track->bus->master summing stage.
Rendered renderBusMix(
    const std::vector<std::vector<misuc::dsp::MidiNote>>& byTrack,
    const std::vector<misuc::engine::MixerTrackParam>& params,
    const std::vector<misuc::engine::MixerBusParam>& buses,
    uint32_t frames) {
  Rendered out;
  out.engine = std::make_unique<misuc::engine::AudioEngine>(
      misuc::test::defaultAudioBackend());
  out.engine->setGroove(false);
  out.engine->setMetronome(false, 0.0f);
  out.engine->setTempo(120.0);
  std::vector<misuc::dsp::MidiNote> flat;
  for (const auto& t : byTrack)
    for (const auto& n : t)
      flat.push_back(n);
  out.engine->setScore(std::move(flat));
  out.engine->publishMixer(params, buses);
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

// Build the published params from a session's gain lanes in track order.
std::vector<misuc::engine::AutomationParam> automationFromSession(
    const misuc::project::Session& s) {
  std::vector<misuc::engine::AutomationParam> out;
  uint32_t idx = 0;
  for (const auto& t : s.tracks()) {
    for (const auto& lane : t.automation) {
      if (lane.param != "gain")
        continue;
      misuc::engine::AutomationParam a;
      a.trackIndex = idx;
      a.active = !lane.muted && !lane.points.empty();
      for (const auto& p : lane.points) {
        a.times.push_back(p.timeBeats);
        a.values.push_back(static_cast<float>(p.value));
      }
      out.push_back(std::move(a));
    }
    ++idx;
  }
  return out;
}

// Steady sine measure on track 0: beats 0..4 at 0 dB, no automation. Used to
// compare window peaks so the automation tests assert relative motion rather
// than absolute oscillator levels.
constexpr uint32_t kBeatsToFrames(double beats) {
  return static_cast<uint32_t>(beats * kFpb120 + 0.5);
}

Rendered steadySine() {
  return renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, 4.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      {}, 4 * kFpb120);
}

// Add a unity "gain" lane with a hard plateau dip (0.45..1.1 beats) to the
// session track and return the new lane id (empty on failure).
std::string engineAddGainLaneWithDip(misuc::project::Session& s,
                                     const std::string& trackId) {
  const std::string lid = s.addAutomationLane(trackId, "gain");
  misuc::project::AutomationLane* lane = s.findAutomationLane(trackId, lid);
  if (!lane)
    return std::string();
  lane->points = {{0.0, 1.0}, {0.5, 1.0}, {0.6, 0.05},
                  {1.1, 0.05}, {1.2, 1.0}, {2.0, 1.0}};
  lane->muted = false;
  return lid;
}

// A minimal pre-automation project document (no "automation" member).
misuc::core::Json legacyTextWithoutAutomation() {
  std::string text = R"({
    "schemaVersion": 2,
    "name": "legacy",
    "tracks": [
      {"id": "t1", "name": "Legacy", "notes": []}
    ]
  })";
  return misuc::core::Json::parse(text);
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
      misuc::test::defaultAudioBackend());
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

// A gain curve that ramps 0.05 -> 1.0 over beat 0..1 then holds must be
// audible: the early window is much quieter than the late window on the same
// steady note, while a unity render stays level across the whole note.
TEST_CASE(automation_gain_curve_audible_in_render) {
  const Rendered unity = steadySine();
  const size_t early0 = kBeatsToFrames(0.25), early1 = kBeatsToFrames(0.45);
  const size_t late0 = kBeatsToFrames(2.50), late1 = kBeatsToFrames(3.50);
  const float uEarly = peakRange(unity.l, early0, early1);
  const float uLate = peakRange(unity.l, late0, late1);
  TEST_ASSERT(uEarly > 0.05f && uLate > 0.05f);
  // Steady note: unity early and late are within 30% of each other.
  TEST_ASSERT(uLate / uEarly > 0.7f && uLate / uEarly < 1.3f);

  Rendered ramp = renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, 4.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      {misuc::engine::AutomationParam{
          0u, true, "gain", std::vector<double>{0.0, 1.0},
          std::vector<float>{0.05f, 1.0f}}},
      4 * kFpb120);
  const float rEarly = peakRange(ramp.l, early0, early1); // ~0.05..~0.7 region
  const float rLate = peakRange(ramp.l, late0, late1);    // holds at 1.0
  TEST_ASSERT(rEarly < uEarly * 0.55f);   // early clearly quieter than unity
  TEST_ASSERT(rLate > uLate * 0.85f);     // late ~ unity (curve flattened)
  TEST_ASSERT(rLate > rEarly * 2.0f);     // provable vertical motion
}

// Points outside the curve hold: before the first point and beyond the last,
// the value stays pinned. A flat two-point curve at 0.5 must halve the note.
TEST_CASE(automation_curve_holds_outside_endpoints) {
  const Rendered unity = steadySine();
  Rendered hold = renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, 4.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      {misuc::engine::AutomationParam{
          0u, true, "gain", std::vector<double>{1.0, 2.0},
          std::vector<float>{0.5f, 0.5f}}},
      4 * kFpb120);
  // Before beat 1 and after beat 2 the 0.5 line still applies (no clamp to 1).
  const float before = peakRange(hold.l, kBeatsToFrames(0.10), kBeatsToFrames(0.90));
  const float after = peakRange(hold.l, kBeatsToFrames(2.10), kBeatsToFrames(3.90));
  const float ref = peakRange(unity.l, kBeatsToFrames(0.10), kBeatsToFrames(0.90));
  TEST_ASSERT(before > 0.05f && after > 0.05f);
  TEST_ASSERT(std::fabs(before / ref - 0.5f) < 0.06f);
  TEST_ASSERT(std::fabs(after / ref - 0.5f) < 0.06f);
}

// A muted lane must publish as inactive and therefore behave as unity gain.
TEST_CASE(automation_muted_lane_is_unity) {
  misuc::project::Session s;
  const std::string tid = s.addTrack("Muted curve");
  const std::string lid = s.addAutomationLane(tid, "gain");
  misuc::project::AutomationLane* lane = s.findAutomationLane(tid, lid);
  TEST_ASSERT(lane != nullptr);
  lane->points = {{0.0, 0.1}, {4.0, 0.1}}; // would be a ~ -20 dB dip
  lane->muted = true;

  const Rendered unity = steadySine();
  Rendered muted = renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, 4.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      automationFromSession(s), 4 * kFpb120);
  const float ref = peakRange(unity.l, kBeatsToFrames(0.5), kBeatsToFrames(3.5));
  const float got = peakRange(muted.l, kBeatsToFrames(0.5), kBeatsToFrames(3.5));
  TEST_ASSERT(got > ref * 0.8f); // muted curve had no effect
}

// Offline export must hear the automation too: renderToWav applies the same
// per-sample curve the live process() path does.
TEST_CASE(automation_export_honors_curve) {
  misuc::project::Session s;
  const std::string tid = s.addTrack("Automated");
  s.findTrack(tid)->notes.push_back(
      misuc::project::NoteEvent{0, 60, 127, 0.0, 2.0}); // beats 0..2

  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(s, notes);

  misuc::engine::AudioEngine engine(
      misuc::test::defaultAudioBackend());
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  engine.setTempo(120.0);
  engine.setScore(std::move(notes));

  const std::string unityPath = "/tmp/misuc_auto_export_unity.wav";
  std::string err;
  TEST_ASSERT(engine.renderToWav(unityPath, 0.0, 2.0, 2, err));

  const std::string lid = engineAddGainLaneWithDip(s, tid);
  TEST_ASSERT(!lid.empty());
  engine.publishAutomation(automationFromSession(s));
  const std::string dipPath = "/tmp/misuc_auto_export_dip.wav";
  TEST_ASSERT(engine.renderToWav(dipPath, 0.0, 2.0, 2, err));

  misuc::audio::AudioBuffer u, d;
  TEST_ASSERT(misuc::audio::readAudioFile(unityPath, u, err));
  TEST_ASSERT(misuc::audio::readAudioFile(dipPath, d, err));
  TEST_ASSERT(u.channels == 2 && d.channels == 2);
  TEST_ASSERT(u.frames == 2 * kFpb120 && d.frames == 2 * kFpb120);

  // Dip plateau at 0.6 -> 1.1 beats: window before (0.15..0.40) ~ unity,
  // window inside (0.65..0.90) dropped hard, window after (1.4..1.95) back.
  auto peak = [](const misuc::audio::AudioBuffer& w, size_t a, size_t b) {
    float p = 0.f;
    for (size_t i = a; i < b && i < w.frames; ++i)
      p = std::max(p, std::fabs(w.samples[i * 2]));
    return p;
  };
  const float uBefore = peak(u, kBeatsToFrames(0.15), kBeatsToFrames(0.40));
  const float dBefore = peak(d, kBeatsToFrames(0.15), kBeatsToFrames(0.40));
  const float dInside = peak(d, kBeatsToFrames(0.65), kBeatsToFrames(0.90));
  const float dAfter = peak(d, kBeatsToFrames(1.40), kBeatsToFrames(1.95));
  TEST_ASSERT(uBefore > 0.05f && dBefore > 0.05f);
  TEST_ASSERT(std::fabs(dBefore / uBefore - 1.0f) < 0.1f); // pre-dip intact
  TEST_ASSERT(dInside < dBefore * 0.35f);                  // dip audible
  TEST_ASSERT(dAfter > dInside * 2.0f);                    // recovered
}

// A Session's gain lane survives the project round-trip: id, param, points
// (sorted) and the mute flag come back unchanged; legacy documents without a
// "automation" member still load with empty lanes.
TEST_CASE(automation_session_roundtrip_and_legacy_docs) {
  misuc::project::Session s;
  const std::string tid = s.addTrack("Rt");
  const std::string lid = s.addAutomationLane(tid, "gain");
  misuc::project::AutomationLane* lane = s.findAutomationLane(tid, lid);
  TEST_ASSERT(lane != nullptr);
  TEST_ASSERT(lane->points.size() == 2); // both endpoints seeded at unity
  lane->points = {{0.0, 1.0}, {0.5, 0.2}, {1.0, 1.0}, {2.5, 0.1}};
  lane->muted = false;

  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  const misuc::project::Track* rt = loaded.findTrack(tid);
  TEST_ASSERT(rt != nullptr);
  TEST_ASSERT(rt->automation.size() == 1);
  const misuc::project::AutomationLane& back = rt->automation.front();
  TEST_ASSERT(back.id == lid);
  TEST_ASSERT(back.param == "gain");
  TEST_ASSERT(!back.muted);
  TEST_ASSERT(back.points.size() == 4);
  TEST_ASSERT(std::fabs(back.points[2].timeBeats - 1.0) < 1e-9);
  TEST_ASSERT(std::fabs(back.points[2].value - 1.0) < 1e-9);
  // Points come back sorted ascending regardless of source order.
  TEST_ASSERT(back.points[1].timeBeats < back.points[2].timeBeats);

  // Hand-written legacy track without "automation": loads, lanes empty.
  const misuc::project::Session legacy =
      misuc::project::Session::fromJson(legacyTextWithoutAutomation().dump());
  TEST_ASSERT(legacy.tracks().size() == 1);
  TEST_ASSERT(legacy.tracks()[0].automation.empty());
}

// A track routed to a bus passes through the bus stage: the bus fader scales
// that track's window while a second track straight to master is untouched.
TEST_CASE(bus_routes_track_and_applies_fader) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});  // routed to bus
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}}); // straight to master

  constexpr uint32_t kFrames = 4 * kFpb120;
  const misuc::engine::MixerTrackParam no{0.f, 0.f, false, false};
  // Unity bus: routed track sounds like itself.
  auto wu = renderBusMix(byTrack,
                         {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0},
                          no},
                         {misuc::engine::MixerBusParam{0.f, 0.f, false, false}},
                         kFrames);
  // -6 dB bus: the routed window drops ~2x, the direct track is untouched.
  auto wd = renderBusMix(byTrack,
                         {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0},
                          no},
                         {misuc::engine::MixerBusParam{-6.f, 0.f, false, false}},
                         kFrames);
  const float au = peakRange(wu.l, 8000, 40000);
  const float bu = peakRange(wu.l, 56000, 88000);
  const float ad = peakRange(wd.l, 8000, 40000);
  const float bd = peakRange(wd.l, 56000, 88000);
  TEST_ASSERT(au > 0.05f && bu > 0.05f);
  // Bus fader is audible on the routed window...
  TEST_ASSERT(ad / au < 0.75f);
  TEST_ASSERT(ad / au > 0.35f); // ~0.5 for -6 dB, smoothing-tolerant
  // ...but cannot reach the track that bypassed it.
  TEST_ASSERT(std::fabs(bd - bu) < 1e-6f);
}

// A soloed bus pulls only its routed (non-muted) tracks through; a direct-to-
// master track falls silent while any solo is active.
TEST_CASE(bus_solo_isolates_routed_tracks) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});  // -> bus 0
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}}); // -> master

  constexpr uint32_t kFrames = 4 * kFpb120;
  const misuc::engine::MixerTrackParam no{0.f, 0.f, false, false};
  auto rs = renderBusMix(byTrack,
                         {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0},
                          no},
                         {misuc::engine::MixerBusParam{0.f, 0.f, false, true}},
                         kFrames);
  const float a = peakRange(rs.l, 8000, 40000);
  const float b = peakRange(rs.l, 56000, 88000);
  TEST_ASSERT(a > 0.05f);  // routed track kept live by its soloed bus
  TEST_ASSERT(b < 1e-5f);  // master-bound track silenced by the active solo
}

// A muted bus eats everything routed into it; the direct track stays live.
TEST_CASE(bus_mute_gates_routed_tracks) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}});

  constexpr uint32_t kFrames = 4 * kFpb120;
  const misuc::engine::MixerTrackParam no{0.f, 0.f, false, false};
  auto rm = renderBusMix(byTrack,
                         {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0},
                          no},
                         {misuc::engine::MixerBusParam{0.f, 0.f, true, false}},
                         kFrames);
  const float a = peakRange(rm.l, 8000, 40000);
  const float b = peakRange(rm.l, 56000, 88000);
  TEST_ASSERT(a < 1e-5f);
  TEST_ASSERT(b > 0.05f);
}

// A hard-panned bus steers the routed signal; the direct track stays centre.
TEST_CASE(bus_pan_steers_routed_track) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack;
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}});
  byTrack.emplace_back(std::vector<misuc::dsp::MidiNote>{
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}});

  constexpr uint32_t kFrames = 4 * kFpb120;
  const misuc::engine::MixerTrackParam no{0.f, 0.f, false, false};
  auto rp = renderBusMix(byTrack,
                         {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0},
                          no},
                         {misuc::engine::MixerBusParam{0.f, -1.f, false, false}},
                         kFrames);
  TEST_ASSERT(peakRange(rp.l, 8000, 40000) > 0.05f);
  TEST_ASSERT(peakRange(rp.r, 8000, 40000) < 1e-5f);
  TEST_ASSERT(peakRange(rp.l, 56000, 88000) > 0.05f); // B unaffected: both live
  TEST_ASSERT(peakRange(rp.r, 56000, 88000) > 0.05f);
}

// Buses survive the project round-trip (name/pan/mute/fader), tracks keep their
// busId, and removeBus re-routes followers back to the master output. Documents
// without a "buses" member load with zero buses.
TEST_CASE(bus_session_roundtrip_and_migration) {
  misuc::project::Session s;
  const std::string bid = s.addBus("Drums");
  const std::string bid2 = s.addBus("Bass");
  const std::string tid = s.addTrack("KickBus");
  s.findTrack(tid)->busId = bid;
  TEST_ASSERT(s.findBus(bid) != nullptr);
  TEST_ASSERT(s.findBus("nope") == nullptr);

  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.buses().size() == 2);
  TEST_ASSERT(loaded.findBus(bid2) != nullptr);
  const misuc::project::Track* back = loaded.findTrack(tid);
  TEST_ASSERT(back != nullptr);
  TEST_ASSERT(back->busId == bid);

  // The top-level "buses" member is absent from pre-bus documents.
  const misuc::project::Session legacy =
      misuc::project::Session::fromJson(legacyTextWithoutAutomation().dump());
  TEST_ASSERT(legacy.buses().empty());
  TEST_ASSERT(legacy.findTrack("t1")->busId.empty());

  // Deleting a bus falls everything back to the master output.
  misuc::project::Session s2;
  const std::string b3 = s2.addBus("One");
  const std::string t3 = s2.addTrack("A");
  s2.findTrack(t3)->busId = b3;
  TEST_ASSERT(s2.removeBus(b3));
  TEST_ASSERT(!s2.removeBus(b3));
  TEST_ASSERT(s2.buses().empty());
  TEST_ASSERT(s2.findTrack(t3)->busId.empty());
  // New ids never collide with the loaded counters.
  const std::string fresh = s2.addBus("Fresh");
  TEST_ASSERT(fresh != b3);
  (void)fresh;
}

// ===========================================================================
// Phase F DoD (mixer scale): a 125-insert session survives the project
// round-trip and the reloaded session renders every one of its 125 inserts and
// 32 destined buses sample-accurately, with the master limiter holding the sum
// under the hard-clip ceiling. publishMixer()/setScore() see the full 125
// indices: the 124th insert's fader/pan/routing lands on RT track 124 and is
// audibly meterable (a cap narrower than 125 would zero it).
// ===========================================================================
TEST_CASE(mixer_scale_125_inserts_render) {
  constexpr uint32_t kInserts = 125;
  constexpr uint32_t kRoutes = 32; // kMaxBusCount
  constexpr uint32_t kFrames = 4 * kFpb120;

  misuc::project::Session s;
  std::vector<std::string> busIds;
  std::vector<std::string> trackIds;
  for (uint32_t b = 0; b < kRoutes; ++b)
    busIds.push_back(s.addBus("Bus " + std::to_string(b + 1)));
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(kInserts);
  for (uint32_t t = 0; t < kInserts; ++t) {
    trackIds.push_back(s.addTrack("Insert " + std::to_string(t + 1)));
    misuc::project::Track* tr = s.findTrack(trackIds.back());
    TEST_ASSERT(tr != nullptr);
    tr->gainDb = (t % 8 == 0) ? -6.f : 0.f;
    tr->pan = (t % 2 == 0) ? -0.6f : 0.6f;
    tr->busId = busIds[t % kRoutes];
    // One distinct-pitch note per insert, held for the whole render, so each
    // insert's post-gain meter proves its own path carried audio.
    byTrack[t].push_back({0.0, 4.0,
                          misuc::dsp::midiNoteToFreq(48 + (t % 24)), 0.9f, t});
  }

  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.tracks().size() == kInserts);
  TEST_ASSERT(loaded.buses().size() == kRoutes);
  for (uint32_t t = 0; t < kInserts; ++t) {
    TEST_ASSERT(loaded.tracks()[t].id == trackIds[t]);
    TEST_ASSERT(loaded.tracks()[t].busId == busIds[t % kRoutes]);
  }

  // Publish the reloaded session exactly as the headroom tool does.
  misuc::engine::AudioEngine engine(
      misuc::test::defaultAudioBackend());
  engine.setTempo(120.0);
  engine.setGroove(false);
  engine.setMetronome(false, 0.0f);
  std::vector<misuc::dsp::MidiNote> flat;
  for (const auto& v : byTrack)
    for (const auto& n : v)
      flat.push_back(n);
  engine.setScore(std::move(flat));

  std::map<std::string, int> busIdx;
  for (uint32_t b = 0; b < loaded.buses().size(); ++b)
    busIdx[loaded.buses()[b].id] = static_cast<int>(b);
  std::vector<misuc::engine::MixerBusParam> buses;
  for (const auto& b : loaded.buses()) {
    misuc::engine::MixerBusParam p;
    p.gainDb = b.faderDb;
    p.pan = b.pan;
    p.mute = b.muted;
    p.solo = b.solo;
    buses.push_back(p);
  }
  std::vector<misuc::engine::MixerTrackParam> tracks;
  for (const auto& t : loaded.tracks()) {
    misuc::engine::MixerTrackParam p;
    p.gainDb = t.gainDb;
    p.pan = t.pan;
    p.mute = t.muted;
    p.solo = t.solo;
    auto it = busIdx.find(t.busId);
    p.busIndex = (t.busId.empty() || it == busIdx.end()) ? -1 : it->second;
    tracks.push_back(p);
  }
  TEST_ASSERT(tracks.size() == kInserts);
  engine.publishMixer(std::move(tracks), std::move(buses));

  engine.play(false);
  std::vector<float> buf(kFrames * 2, 0.f);
  engine.process(nullptr, 0, buf.data(), 2, kFrames, 0, kSr);

  uint32_t audible = 0;
  for (uint32_t t = 0; t < kInserts; ++t)
    if (engine.trackMeterPeakL(t) > 1e-4f || engine.trackMeterPeakR(t) > 1e-4f)
      ++audible;
  TEST_ASSERT(audible == kInserts);
  for (uint32_t b = 0; b < kRoutes; ++b)
    TEST_ASSERT(engine.busMeterPeakL(b) > 1e-4f ||
                engine.busMeterPeakR(b) > 1e-4f);

  uint64_t hard = 0;
  for (float v : buf)
    if (std::fabs(v) > 0.999f)
      ++hard;
  TEST_ASSERT(hard == 0);                 // master limiter: no hard clip
  TEST_ASSERT(engine.meterPeakL() > 0.01f); // master actually moving
}

// ── Phase L: pan/volume automation ────────────────────────────────────────

// Pan automation: a constant pan curve (-1..+1) across two halves should show
// the left channel dominating in the first half and the right in the second.
TEST_CASE(mixer_pan_automation_sweeps_lr) {
  constexpr uint32_t kFrames = 4 * kFpb120;
  const double kLen = 4.0;
  Rendered withPan = renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, kLen, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      // Track 0, pan curve: hard left for 2 beats, hard right for 2 beats.
      {misuc::engine::AutomationParam{0, true, "pan",
          std::vector<double>{0.0, 1.99, 2.01, kLen},
          std::vector<float>{-1.0f, -1.0f, 1.0f, 1.0f}}},
      kFrames);

  // Beats 0..2 (window 0.2..1.8): hard-left pan — left channel dominant.
  const uint32_t w0 = kBeatsToFrames(0.2), w1 = kBeatsToFrames(1.8);
  const float l0 = peakRange(withPan.l, w0, w1);
  const float r0 = peakRange(withPan.r, w0, w1);
  TEST_ASSERT(l0 > 0.05f && r0 < 1e-4f);

  // Beats 2..4 (window 2.2..3.8): hard-right pan — right channel dominant.
  const uint32_t w2 = kBeatsToFrames(2.2), w3 = kBeatsToFrames(3.8);
  const float l1 = peakRange(withPan.l, w2, w3);
  const float r1 = peakRange(withPan.r, w2, w3);
  TEST_ASSERT(r1 > 0.05f && l1 < 1e-4f);
}

// Volume automation: half the amplitude in the second half produces a
// measurably smaller peak (volume [0,1] vs gain 0..2 — volume applies in
// series with the default unity gain).
TEST_CASE(mixer_volume_automation_halves_peak) {
  constexpr uint32_t kFrames = 4 * kFpb120;
  const double kLen = 4.0;
  Rendered fullVol = renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, kLen, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      {}, // no automation
      kFrames);
  Rendered halfVol = renderAuto(
      {std::vector<misuc::dsp::MidiNote>{
          {0.0, kLen, misuc::dsp::midiNoteToFreq(60), 1.f, 0}}},
      {misuc::engine::AutomationParam{0, true, "volume",
          std::vector<double>{0.0, 1.99, 2.01, kLen},
          std::vector<float>{1.0f, 1.0f, 0.5f, 0.5f}}},
      kFrames);

  // Beats 2..4 at volume 0.5 (window 2.2..3.8) should be roughly half the
  // amplitude of the no-automation case (within ±10 % to tolerate
  // interpolation smoothing).
  const uint32_t v0 = kBeatsToFrames(2.2), v1 = kBeatsToFrames(3.8);
  const float p1 = peakRange(fullVol.l, v0, v1);
  const float p2 = peakRange(halfVol.l, v0, v1);
  TEST_ASSERT(p1 > 0.05f);
  // p2 should be ~0.5 * p1.
  TEST_ASSERT(std::fabs(p2 / p1 - 0.5f) < 0.1f);
}