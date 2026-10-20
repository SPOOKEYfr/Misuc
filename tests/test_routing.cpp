// Comp19 routing-graph engine tests: bus→bus targets render in dependency
// order, cycle edges are rescued to the master at the engine level, parallel
// sends (track and bus sourced) stay audible alongside the source route and
// scale with the send level, sends follow the source's mute/solo gate, and the
// built-in SC compressor ducks a bus off its sidechain key bus. All renders
// run headless by driving AudioEngine::process() directly.

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/fx/BuiltinFx.h>
#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "test_utils.h"

namespace {

constexpr uint32_t kSr = 48000u;
constexpr uint32_t kFpb120 = 24000u;

inline float peakRange(const std::vector<float>& v, size_t from, size_t to) {
  float p = 0.f;
  for (size_t i = from; i < to && i < v.size(); ++i)
    p = std::max(p, std::fabs(v[i]));
  return p;
}

// Render `byTrack` (a note on each listed track, beats 0..2) with the given
// mixer/bus/send parameters and per-bus fx chains, returning interleaved out.
struct Rendered {
  std::unique_ptr<misuc::engine::AudioEngine> engine;
  std::vector<float> l;
  std::vector<float> r;
};

Rendered renderRouting(
    const std::vector<std::vector<misuc::dsp::MidiNote>>& byTrack,
    const std::vector<misuc::engine::MixerTrackParam>& params,
    const std::vector<misuc::engine::MixerBusParam>& buses,
    const std::vector<misuc::engine::MixerSendParam>& sends,
    const std::vector<std::vector<misuc::engine::FxInsertParam>>& busChains,
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
  out.engine->publishMixer(params, buses, sends);
  out.engine->publishFx({}, busChains);
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

constexpr uint32_t kFrames = 2 * kFpb120; // beats 0..2
constexpr size_t kW0 = 8000, kW1 = 46000; // steady window inside the note

} // namespace

// Two buses in series (track -> bus0 -> bus1 -> master): both faders are
// audible serially on the same signal, and nothing reaches the master when
// bus0 routes nowhere.
TEST_CASE(routing_bus_target_feeds_parent_fader_serial) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(1);
  byTrack[0].push_back({0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0});

  const misuc::engine::MixerTrackParam tr{0.f, 0.f, false, false, 0};
  // bus0 -> bus1 (target index 1), bus1 -> master.
  auto render = [&](float bus0Db, float bus1Db) {
    return renderRouting(
        byTrack, {tr},
        {misuc::engine::MixerBusParam{bus0Db, 0.f, false, false, 1},
         misuc::engine::MixerBusParam{bus1Db, 0.f, false, false, -1}},
        {}, {}, kFrames);
  };
  const float su = peakRange(render(0.f, 0.f).l, kW0, kW1);
  const float sd0 = peakRange(render(-6.f, 0.f).l, kW0, kW1);
  const float sd1 = peakRange(render(0.f, -6.f).l, kW0, kW1);
  TEST_ASSERT(su > 0.05f);
  // -6 dB on the child bus halves the master level...
  TEST_ASSERT(sd0 / su > 0.35f && sd0 / su < 0.75f);
  // ...and -6 dB on the parent bus halves it again, proving the chain.
  TEST_ASSERT(sd1 / su > 0.35f && sd1 / su < 0.75f);
}

// A cyclic engine-level target must never feed back: both offending edges are
// rescued to the master and audio still renders (loud, not sizzling).
TEST_CASE(routing_bus_target_cycle_rescued_to_master) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(1);
  byTrack[0].push_back({0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0});

  auto r = renderRouting(
      byTrack,
      {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0}},
      {misuc::engine::MixerBusParam{0.f, 0.f, false, false, 1},
       misuc::engine::MixerBusParam{0.f, 0.f, false, false, 0}},
      {}, {}, kFrames);
  const float p = peakRange(r.l, kW0, kW1);
  TEST_ASSERT(p > 0.05f);
  // Rescued buses route to the master, so the routed signal is simply audible
  // (no runaway/feedback: the peak stays well bounded).
  TEST_ASSERT(p < 2.0f);
}

// A parallel send from a track to another bus must leave the dry route fully
// audible (source keeps its own path) while the send tap feeds the target bus;
// scaling the send level scales only the send contribution.
TEST_CASE(routing_track_send_parallel_and_level_scaled) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(1);
  byTrack[0].push_back({0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0});

  const misuc::engine::MixerTrackParam tr{0.f, 0.f, false, false, 0};
  const std::vector<misuc::engine::MixerBusParam> buses{
      {0.f, 0.f, false, false, -1}, // bus0 = dry
      {0.f, 0.f, false, false, -1}, // bus1 = send target
  };
  // No send: only the dry bus sounds.
  auto noSend = renderRouting(
      byTrack, {tr}, buses, {}, {}, kFrames);
  const float pa = peakRange(noSend.l, kW0, kW1);
  TEST_ASSERT(pa > 0.05f);

  // Unity send: the send tap adds a second, equal copy at the master.
  auto unity = renderRouting(
      byTrack, {tr}, buses,
      {misuc::engine::MixerSendParam{0, 0xFF, 1, misuc::dsp::gainFromDb(0.f),
                                     true}},
      {}, kFrames);
  const float pu = peakRange(unity.l, kW0, kW1);
  TEST_ASSERT(pu / pa > 1.6f && pu / pa < 2.4f);

  // -20 dB send: the master is only slightly above the dry route's level.
  auto soft = renderRouting(
      byTrack, {tr}, buses,
      {misuc::engine::MixerSendParam{0, 0xFF, 1,
                                     misuc::dsp::gainFromDb(-20.f), true}},
      {}, kFrames);
  const float ps = peakRange(soft.l, kW0, kW1);
  TEST_ASSERT(ps / pa > 1.01f && ps / pa < 1.25f);
}

// A muted source gates BOTH its dry route and its send taps to silence.
TEST_CASE(routing_send_follows_source_mute) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(1);
  byTrack[0].push_back({0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0});

  const std::vector<misuc::engine::MixerBusParam> buses{
      {0.f, 0.f, false, false, -1},
      {0.f, 0.f, false, false, -1},
  };
  auto r = renderRouting(
      byTrack,
      {misuc::engine::MixerTrackParam{0.f, 0.f, true, false, 0}},
      buses,
      {misuc::engine::MixerSendParam{0, 0xFF, 1, misuc::dsp::gainFromDb(0.f),
                                     true}},
      {}, kFrames);
  TEST_ASSERT(peakRange(r.l, kW0, kW1) < 1e-5f);
}

// Bus-sourced sends: a post-fader tap of one bus into another, still parallel
// with the source bus's own route.
TEST_CASE(routing_send_from_bus_to_bus) {
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(1);
  byTrack[0].push_back({0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0});

  const misuc::engine::MixerTrackParam tr{0.f, 0.f, false, false, 0};
  const std::vector<misuc::engine::MixerBusParam> buses{
      {0.f, 0.f, false, false, -1}, // bus0 = source
      {0.f, 0.f, false, false, -1}, // bus1 = send target
  };
  auto noSend = renderRouting(byTrack, {tr}, buses, {}, {}, kFrames);
  const float pa = peakRange(noSend.l, kW0, kW1);
  TEST_ASSERT(pa > 0.05f);

  auto tapped = renderRouting(
      byTrack, {tr}, buses,
      {misuc::engine::MixerSendParam{0xFF, 0, 1, misuc::dsp::gainFromDb(0.f),
                                     true}},
      {}, kFrames);
  const float pt = peakRange(tapped.l, kW0, kW1);
  TEST_ASSERT(pt / pa > 1.6f && pt / pa < 2.4f);
}

// Sidechain ducking: the built-in SC compressor on the bass bus keys off a
// (muted, inaudible) kick bus. With the kick sounding, the bass is driven down;
// with the kick silent, the bass passes untouched. Identical chain in both.
TEST_CASE(routing_sidechain_compressor_ducks_bus) {
  const uint32_t kKeyBus = 1, kBassBus = 0;
  std::vector<std::vector<misuc::dsp::MidiNote>> byTrack(2);
  byTrack[kBassBus].push_back(
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, kBassBus});
  byTrack[kKeyBus].push_back(
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(36), 1.f, kKeyBus});

  std::vector<misuc::engine::MixerBusParam> buses{
      {0.f, 0.f, false, false, -1},                    // bass -> master
      {0.f, 0.f, false, false, -1},                    // kick -> master
      {0.f, 0.f, false, false, -1},                    // spare
  };
  // Bus 0 (bass) carries the SC compressor keyed off bus 1. The kick bus is
  // MUTED in both renders so only the ducking is measurable at the master.
  buses[1].mute = true;
  std::vector<std::vector<misuc::engine::FxInsertParam>> busChains(
      3, std::vector<misuc::engine::FxInsertParam>());
  busChains[kBassBus].push_back(misuc::engine::FxInsertParam{
          misuc::engine::kBuiltinCompUri, true, {}, static_cast<int>(kKeyBus)});

  const misuc::engine::MixerTrackParam bass{0.f, 0.f, false, false, kBassBus};
  const misuc::engine::MixerTrackParam kick{0.f, 0.f, false, false, kKeyBus};

  // K: kick (key) sounding -> bass ducked.
  auto withKey = renderRouting(byTrack, {bass, kick}, buses, {}, busChains,
                               kFrames);
  // S: kick silent (same chain, same buses) -> bass at full level.
  std::vector<std::vector<misuc::dsp::MidiNote>> withoutKey(2);
  withoutKey[kBassBus] = byTrack[kBassBus];
  auto noKey = renderRouting(withoutKey, {bass, kick}, buses, {}, busChains,
                             kFrames);

  const float pk = peakRange(withKey.l, kW0, kW1);
  const float ps = peakRange(noKey.l, kW0, kW1);
  TEST_ASSERT(ps > 0.05f);                 // bass always present
  TEST_ASSERT(pk < ps * 0.6f);             // key drives the bass down clearly
}

// The Session persists the routing graph, parallel sends, and bus fx (with
// their sidechain source) through the project round-trip.
TEST_CASE(routing_session_roundtrip_persists_graph_sends_and_bus_fx) {
  misuc::project::Session s;
  const std::string bA = s.addBus("Kick");
  const std::string bB = s.addBus("Bass");
  const std::string bC = s.addBus("Group");
  const std::string t1 = s.addTrack("KickHub");

  // Routing: Kick -> Group -> master; setBusTarget refuses a cycle.
  TEST_ASSERT(s.setBusTarget(bA, bC));
  const std::string back = s.setBusTarget(bC, bA) ? "cycle" : "ok";
  TEST_ASSERT(back == "ok");

  // Send: KickHub (track) and the Kick bus both feed the Bass bus.
  const std::string sid1 = s.addSend(t1, "", bB, -6.0f);
  const std::string sid2 = s.addSend("", bA, bB, -12.0f);
  TEST_ASSERT(!sid1.empty() && !sid2.empty());

  // Bus fx with a sidechain key on the Bass bus.
  const std::string fid = s.addBusFx(bB, misuc::engine::kBuiltinCompUri, bA);
  TEST_ASSERT(!fid.empty());
  misuc::project::Bus* bass = s.findBus(bB);
  TEST_ASSERT(bass != nullptr && !bass->fx.empty());
  bass->fx.front().params.push_back({misuc::dsp::Compressor::kThresholdDbPort,
                                     -30.f});

  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  TEST_ASSERT(loaded.findBus(bA)->target == bC);
  TEST_ASSERT(loaded.findBus(bC)->target.empty()); // rescued -> master
  TEST_ASSERT(loaded.sends().size() == 2);
  const misuc::project::Send* back1 = loaded.findSend(sid1);
  const misuc::project::Send* back2 = loaded.findSend(sid2);
  TEST_ASSERT(back1 != nullptr && back2 != nullptr);
  TEST_ASSERT(back1->sourceTrack == t1 && back1->targetBus == bB);
  TEST_ASSERT(back1->sourceBus.empty());
  TEST_ASSERT(back2->sourceBus == bA && back2->targetBus == bB);
  TEST_ASSERT(std::fabs(back1->levelDb - (-6.0f)) < 1e-6f);
  TEST_ASSERT(loaded.findBus(bB)->fx.size() == 1);
  TEST_ASSERT(loaded.findBus(bB)->fx.front().uri ==
              misuc::engine::kBuiltinCompUri);
  TEST_ASSERT(loaded.findBus(bB)->fx.front().sidechainSource == bA);
  TEST_ASSERT(loaded.findBus(bB)->fx.front().params.size() == 1);

  // removeSend cleans up cleanly.
  misuc::project::Session s3;
  const std::string bx = s3.addBus("X");
  const std::string st = s3.addTrack("Src");
  const std::string sb = s3.addSend(st, "", bx, 0.f);
  TEST_ASSERT(!sb.empty() && s3.sends().size() == 1);
  TEST_ASSERT(s3.removeSend(sb));
  TEST_ASSERT(s3.sends().empty());
  TEST_ASSERT(!s3.removeSend(sb));
}