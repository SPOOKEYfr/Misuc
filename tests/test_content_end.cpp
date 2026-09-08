// Content-end semantics: Session::contentEndBeats() derives the effective
// arrangement end from actual content (audio clips, MIDI regions, legacy
// notes), and the engine stops normal playback at that beat — while loop
// playback keeps spinning its own range and unmatched sessions keep the
// historic free-run behaviour. All engine renders run headless by driving
// AudioEngine::process() directly.

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/project/Session.h>

#include <memory>
#include <vector>

#include "test_utils.h"

using misuc::project::Session;
using misuc::engine::AudioEngine;

namespace {

constexpr uint32_t kSr = 48000u;
// 120 bpm => 24000 frames per beat.
constexpr uint32_t kFpb120 = 24000u;
constexpr uint32_t kChunk = 512u;

std::unique_ptr<AudioEngine> makeEngine() {
  auto e = std::make_unique<AudioEngine>(
      std::make_unique<misuc::engine::PipeWireBackend>());
  e->setGroove(false);
  e->setMetronome(false, 0.0f);
  e->setTempo(120.0);
  return e;
}

// Drives `blocks` process calls of kChunk frames through the engine.
void spin(AudioEngine& e, uint32_t blocks) {
  std::vector<float> buf(kChunk * 2, 0.f);
  for (uint32_t b = 0; b < blocks; ++b)
    e.process(nullptr, 0, buf.data(), 2, kChunk, 0, kSr);
}

} // namespace

// ── Session::contentEndBeats ─────────────────────────────────────────────

TEST_CASE(session_content_end_empty_is_zero) {
  Session s;
  TEST_ASSERT(s.contentEndBeats() == 0.0);
  s.addTrack("Drums");
  TEST_ASSERT(s.contentEndBeats() == 0.0);
}

TEST_CASE(session_content_end_covers_clips_regions_notes) {
  Session s;
  const std::string tid = s.addTrack("Drums");
  TEST_ASSERT(!tid.empty());

  // Empty track, no content.
  TEST_ASSERT(s.contentEndBeats() == 0.0);

  // Audio clip reaching beat 9.
  misuc::project::AudioClip c;
  c.sourceFile = "/tmp/never-read.wav";
  c.startBeats = 4.0;
  c.durationBeats = 5.0;
  s.addClip(tid, std::move(c));
  TEST_ASSERT(s.contentEndBeats() == 9.0);

  // Region reaching beat 12; note overhang reaches 14.
  const std::string rid = s.addRegion(tid, "Pattern", 10.0, 2.0);
  TEST_ASSERT(!rid.empty());
  TEST_ASSERT(s.contentEndBeats() == 12.0);
  misuc::project::MidiRegion* r = s.findRegion(tid, rid);
  TEST_ASSERT(r != nullptr);
  misuc::project::NoteEvent n;
  n.startBeats = 0.5;
  n.durationBeats = 3.5; // 10 + 0.5 + 3.5 = 14 > region width
  r->notes.push_back(n);
  TEST_ASSERT(s.contentEndBeats() == 14.0);

  // A second track with legacy (whole-track) notes.
  const std::string tid2 = s.addTrack("Bass");
  s.tracks()[1].notes.push_back(misuc::project::NoteEvent{0, 40, 100, 20.0, 4.0});
  TEST_ASSERT(s.contentEndBeats() == 24.0);

  // Removing all content returns to zero.
  s.removeTrack(tid);
  s.removeTrack(tid2);
  TEST_ASSERT(s.contentEndBeats() == 0.0);
}

// ── Engine: stops at the published arrangement end ──────────────────────

TEST_CASE(engine_empty_project_does_not_play) {
  auto e = makeEngine();
  e->setContentEnd(0.0);
  e->play(false);
  spin(*e, 8);
  TEST_ASSERT(!e->isTransportPlaying());
  TEST_ASSERT(e->playheadSamples() == 0);
}

TEST_CASE(engine_stops_at_content_end) {
  auto e = makeEngine();
  const uint64_t endFrames = static_cast<uint64_t>(4.0 * kFpb120);
  e->setContentEnd(4.0);
  e->play(false);
  // Enough chunks to run past beat 4 at 120 bpm (96000 frames):
  spin(*e, static_cast<uint32_t>(220));
  TEST_ASSERT(!e->isTransportPlaying());
  TEST_ASSERT(e->playheadSamples() == endFrames);
}

TEST_CASE(engine_seek_past_end_stops_immediately) {
  auto e = makeEngine();
  const uint64_t endFrames = static_cast<uint64_t>(4.0 * kFpb120);
  e->setContentEnd(4.0);
  e->seekToBeat(10.0); // caret parked beyond the content
  spin(*e, 1);
  TEST_ASSERT(!e->isTransportPlaying());
  e->play(false);
  spin(*e, 2);
  TEST_ASSERT(!e->isTransportPlaying());
  TEST_ASSERT(e->playheadSamples() == endFrames);
}

TEST_CASE(engine_loop_overrides_content_end) {
  auto e = makeEngine();
  e->setContentEnd(4.0);
  e->setLoop(true, 0.0, 4.0);
  e->play(false);
  // Massively past the content end: loop keeps spinning within [0, 4).
  spin(*e, 400);
  TEST_ASSERT(e->isTransportPlaying());
  const uint64_t loopEndFrames = static_cast<uint64_t>(4.0 * kFpb120);
  TEST_ASSERT(e->playheadSamples() < loopEndFrames);
}

TEST_CASE(engine_unpublished_content_end_runs_free) {
  // No setContentEnd() call: legacy (unmatched test harness / pattern-play)
  // sessions keep the historic behaviour of playing without an end limit.
  auto e = makeEngine();
  e->play(false);
  spin(*e, 200); // 200 * 512 = 102400 frames > 4 beats
  TEST_ASSERT(e->isTransportPlaying());
  TEST_ASSERT(e->playheadSamples() > static_cast<uint64_t>(4.0 * kFpb120));
}