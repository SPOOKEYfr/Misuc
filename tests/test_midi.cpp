#include <cmath>
#include <cstdio>
#include <vector>

#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

#include "test_utils.h"

using misuc::dsp::MidiNote;
using misuc::dsp::MidiPlayer;
using misuc::dsp::midiNoteToFreq;
using misuc::engine::buildMidiScore;
using misuc::project::NoteEvent;
using misuc::project::Session;
using misuc::project::Track;

namespace {
constexpr double kSr = 48000.0;
constexpr double kFpb120 = (60.0 / 120.0) * kSr; // 24000 frames/beat @120bpm

std::vector<float> renderRange(MidiPlayer& p,
                               const std::vector<MidiNote>& score,
                               uint64_t startFrame, uint32_t frames,
                               uint64_t wallFrame = 0) {
  std::vector<float> buf(frames, 0.f);
  p.setSampleRate(kSr);
  p.setTempo(120.0);
  // stride = 1 for mono readability; player adds to outL with outR==outL.
  p.render(score, true, buf.data(), buf.data(), frames, 1, startFrame,
           wallFrame);
  return buf;
}

float peak(const std::vector<float>& v) {
  float m = 0.f;
  for (float x : v)
    m = std::max(m, std::fabs(x));
  return m;
}
} // namespace

TEST_CASE(midi_note_freq) {
  TEST_ASSERT(std::fabs(midiNoteToFreq(69) - 440.0) < 1e-6);
  TEST_ASSERT(std::fabs(midiNoteToFreq(60) - 261.625565) < 1e-3);
  TEST_ASSERT(std::fabs(midiNoteToFreq(81) - 880.0) < 1e-6);
}

TEST_CASE(midi_note_plays_in_its_block) {
  MidiPlayer p;
  std::vector<MidiNote> score{{0.0, 2.0, 440.0, 1.f}};
  const auto buf = renderRange(p, score, 0, 4096);
  TEST_ASSERT(peak(buf) > 1e-3f);
  TEST_ASSERT(buf[0] < 1e-9f); // silent at the very first sample (attack=0)
}

TEST_CASE(midi_quiet_before_and_after_note) {
  MidiPlayer p;
  std::vector<MidiNote> score{{4.0, 1.0, 440.0, 1.f}};
  const auto before = renderRange(p, score, 0, static_cast<uint32_t>(kFpb120));
  TEST_ASSERT(peak(before) < 1e-9f); // no note overlap -> zero output
  const auto after = renderRange(
      p, score, static_cast<uint64_t>(5 * kFpb120 + 4 * kSr * 0.090) + 4096,
      4096);
  TEST_ASSERT(peak(after) < 1e-9f); // well past release tail
}

TEST_CASE(midi_release_tail_decays) {
  MidiPlayer p;
  std::vector<MidiNote> score{{0.0, 0.25, 440.0, 1.f}};
  const uint64_t noteEnd = static_cast<uint64_t>(0.25 * kFpb120);
  const uint32_t tailLen = static_cast<uint32_t>(kSr * 0.27); // ~3x release tau
  const auto tail = renderRange(p, score, noteEnd, tailLen);
  TEST_ASSERT(peak(tail) > 1e-3f);
  // The exponential release envelope must lose almost all energy by 3 tau
  // (the raw samples oscillate, so compare window peaks).
  const float earlyPeak =
      peak(std::vector<float>(tail.begin(), tail.begin() + 2048));
  const float latePeak =
      peak(std::vector<float>(tail.end() - 2048, tail.end()));
  TEST_ASSERT(earlyPeak > 10.f * latePeak);
  TEST_ASSERT(latePeak < 0.02f);
}

TEST_CASE(midi_block_boundary_seamless) {
  MidiPlayer split;
  MidiPlayer whole;
  std::vector<MidiNote> score{{0.0, 8.0, 220.0, 0.8f}};
  auto b1 = renderRange(split, score, 0, 1000);
  auto b2 = renderRange(split, score, 1000, 1000);
  auto big = renderRange(whole, score, 0, 2000);
  for (uint32_t i = 0; i < 2000; ++i) {
    const float splitVal = i < 1000 ? b1[i] : b2[i - 1000];
    TEST_ASSERT(splitVal == big[i]); // bit-identical seams
  }
}

TEST_CASE(midi_loop_rewind_is_deterministic) {
  MidiPlayer rewound;
  MidiPlayer fresh;
  std::vector<MidiNote> score{{0.0, 8.0, 330.0, 0.7f}};
  (void)renderRange(rewound, score, 5000, 10000); // play forward...
  auto rb = renderRange(rewound, score, 0, 1000); // ...then rewind
  auto fb = renderRange(fresh, score, 0, 1000);
  for (uint32_t i = 0; i < 1000; ++i)
    TEST_ASSERT(rb[i] == fb[i]); // restart is bit-identical
}

TEST_CASE(midi_polyphony_adds) {
  MidiPlayer bothP, aP, bP;
  std::vector<MidiNote> a{{0.0, 4.0, 261.625565, 0.5f}};
  std::vector<MidiNote> b{{0.0, 4.0, 440.0, 0.5f}};
  std::vector<MidiNote> ab{{0.0, 4.0, 261.625565, 0.5f},
                           {0.0, 4.0, 440.0, 0.5f}};
  auto va = renderRange(aP, a, 0, 1024);
  auto vb = renderRange(bP, b, 0, 1024);
  auto vab = renderRange(bothP, ab, 0, 1024);
  for (uint32_t i = 0; i < 1024; ++i)
    TEST_ASSERT(vab[i] == va[i] + vb[i]);
}

TEST_CASE(midi_score_build_mapping) {
  Session s;
  const std::string id = s.addTrack("Lead");
  const std::string mutedId = s.addTrack("Silent");
  Track* lead = s.findTrack(id);
  Track* silent = s.findTrack(mutedId);
  lead->gainDb = 6.f; // +6 dB -> gain 2.0
  lead->notes.push_back(NoteEvent{0, 60, 127, 0.0, 1.0});
  lead->notes.push_back(NoteEvent{0, 60, 64, 0.5, 0.5});
  silent->muted = true;
  silent->notes.push_back(NoteEvent{0, 72, 127, 0.0, 1.0});

  std::vector<MidiNote> score;
  buildMidiScore(s, score);
  TEST_ASSERT(score.size() == 2); // muted track excluded
  TEST_ASSERT(std::fabs(midiNoteToFreq(60) - score[0].freq) < 1e-6);
  TEST_ASSERT(score[0].startBeats < score[1].startBeats); // sorted
  // Track gain +6 dB (1.995) is baked into every note with velocity scaling.
  const float db6 = misuc::dsp::gainFromDb(6.f);
  TEST_ASSERT(std::fabs(score[0].gain - (127.f / 127.f) * db6) < 1e-4f);
  TEST_ASSERT(std::fabs(score[1].gain - (64.f / 127.f) * db6) < 1e-4f);
}

TEST_CASE(midi_empty_score_silent) {
  MidiPlayer p;
  std::vector<MidiNote> score;
  const auto buf = renderRange(p, score, 0, 4096);
  TEST_ASSERT(peak(buf) < 1e-9f);
}

TEST_CASE(midi_player_respects_tempo) {
  // Same score under two tempos must reach the note at different frames.
  const uint64_t beat1At120 = static_cast<uint64_t>(kFpb120);
  std::vector<MidiNote> score{{1.0, 4.0, 440.0, 1.f}};

  MidiPlayer p120;
  p120.setSampleRate(kSr);
  p120.setTempo(120.0);
  std::vector<float> buf120(256, 0.f);
  p120.render(score, true, buf120.data(), buf120.data(), 256, 1,
              beat1At120, 0);

  MidiPlayer p60;
  p60.setSampleRate(kSr);
  p60.setTempo(60.0);
  std::vector<float> buf60(256, 0.f);
  p60.render(score, true, buf60.data(), buf60.data(), 256, 1,
             beat1At120, 0);

  TEST_ASSERT(peak(buf120) > 1e-3f);            // 120bpm: beat 1 already here
  TEST_ASSERT(peak(buf60) < 1e-9f);             // 60bpm: beat 1 is later
}

TEST_CASE(midi_preview_sounds_on_wall_clock) {
  // Audition notes play on the wall timebase even with no score and while the
  // transport would be stopped (includeScore=false).
  MidiPlayer p;
  p.setSampleRate(kSr);
  p.setTempo(120.0);
  p.triggerPreview(1000.0, 0.35 * kSr, 440.0, 1.f);

  std::vector<float> buf(4096, 0.f);
  p.render(std::vector<MidiNote>{}, false, buf.data(), buf.data(), 4096, 1, 0,
           0);

  float before = 0.f;
  for (size_t i = 0; i < 1000; ++i)
    before = std::max(before, std::fabs(buf[i]));
  const float after = peak(buf);
  TEST_ASSERT(before < 1e-9f); // silent until the preview starts
  TEST_ASSERT(after > 1e-2f);
}

TEST_CASE(midi_preview_decays_after_duration) {
  MidiPlayer p;
  p.setSampleRate(kSr);
  p.setTempo(120.0);
  const double dur = 0.35 * kSr; // 16800 frames
  p.triggerPreview(0.0, dur, 440.0, 1.f);

  // Render well past the note end + release => must be silent again.
  std::vector<float> buf(8192, 0.f);
  p.render(std::vector<MidiNote>{}, false, buf.data(), buf.data(), 8192, 1, 0,
           38000);
  TEST_ASSERT(peak(buf) < 1e-6f);
}

TEST_CASE(midi_preview_frequency_matches) {
  // A preview at 2x the frequency must cross zero at ~2x the rate.
  const auto crossings = [](double srNow, double freq) {
    MidiPlayer p;
    p.setSampleRate(srNow);
    p.setTempo(120.0);
    p.triggerPreview(0.0, srNow, freq, 1.f);
    std::vector<float> buf(static_cast<size_t>(srNow), 0.f);
    p.render(std::vector<MidiNote>{}, false, buf.data(), buf.data(),
             static_cast<uint32_t>(srNow), 1, 0, 0);
    int n = 0;
    for (size_t i = 12000; i + 1 < 36000; ++i)
      if ((buf[i] > 0.f && buf[i + 1] <= 0.f) ||
          (buf[i] < 0.f && buf[i + 1] >= 0.f))
        ++n;
    return n;
  };
  const int c440 = crossings(kSr, 440.0);
  const int c880 = crossings(kSr, 880.0);
  TEST_ASSERT(c440 > 400);
  TEST_ASSERT(c880 > c440);
  const double ratio = static_cast<double>(c880) / c440;
  TEST_ASSERT(ratio > 1.4 && ratio < 2.8);
}

TEST_CASE(midi_score_and_preview_add) {
  // Same pitch in both timebases at overlapping times lands in phase, so the
  // combined energy is ~4x a lone note in the steady region.
  const std::vector<MidiNote> score{{0.0, 4.0, 220.0, 1.f}};

  MidiPlayer alone;
  alone.setSampleRate(kSr);
  alone.setTempo(120.0);
  std::vector<float> a(16000, 0.f);
  alone.render(score, true, a.data(), a.data(), 16000, 1, 0, 0);

  MidiPlayer both;
  both.setSampleRate(kSr);
  both.setTempo(120.0);
  both.triggerPreview(0.0, 0.35 * kSr, 220.0, 1.f);
  std::vector<float> b(16000, 0.f);
  both.render(score, true, b.data(), b.data(), 16000, 1, 0, 0);

  const auto en = [](const std::vector<float>& v, size_t from, size_t to) {
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
      s += static_cast<double>(v[i]) * v[i];
    return s;
  };
  const double ea = en(a, 4000, 12000);
  const double eb = en(b, 4000, 12000);
  TEST_ASSERT(ea > 1e-4);
  TEST_ASSERT(eb > 3.0 * ea);
}

TEST_CASE(midi_preview_ring_overwrites_oldest) {
  // The preview ring holds kPreviewSlots notes; pushing one more than the ring
  // size evicts the oldest, which here is identical, so the energy of a 9th
  // push equals that of exactly 8 pushes.
  const auto renderPushed = [](int pushes) {
    MidiPlayer p;
    p.setSampleRate(kSr);
    p.setTempo(120.0);
    for (int i = 0; i < pushes; ++i)
      p.triggerPreview(0.0, 0.35 * kSr, 220.0, 1.f);
    std::vector<float> buf(8192, 0.f);
    p.render(std::vector<MidiNote>{}, false, buf.data(), buf.data(), 8192, 1,
             0, 0);
    double s = 0.0;
    for (float v : buf)
      s += static_cast<double>(v) * v;
    return s;
  };
  const double e8 = renderPushed(8);
  const double e9 = renderPushed(9);
  TEST_ASSERT(e8 > 1e-4);
  TEST_ASSERT(std::fabs(e9 - e8) < e8 * 1e-6);
}