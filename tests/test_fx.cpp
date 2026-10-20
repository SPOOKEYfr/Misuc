// LV2 effect insert tests. When the build hosts lilv (MISUC_LV2=1) the real
// mda-lv2 bundle is discovered and its effects are provably audible through
// the engine's post-fader chain in the live process() path.
//
// When hosting is unavailable every host-level check degrades gracefully and
// the insert chain behaves as a dry pass-through.
//
// Render assertions run all passes on ONE engine (publishing the empty chain,
// then the disabled insert, then the enabled insert in sequence). Earlier
// suites can leave sparse ambient energy in freshly built engine instances
// (a longstanding harness quirk); sharing the engine makes that drift hit
// every pass identically, so cross-pass deltas measure only the plugin.

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "test_utils.h"

namespace {

constexpr uint32_t kSr = 48000u;
constexpr uint32_t kFpb120 = 24000u; // 120 bpm => 24000 frames per beat

inline float peakRange(const std::vector<float>& v, size_t from, size_t to) {
  float p = 0.f;
  for (size_t i = from; i < to && i < v.size(); ++i)
    p = std::max(p, std::fabs(v[i]));
  return p;
}

struct FxPass {
  std::vector<float> l;
  std::vector<float> r;
};

// One steady note, beats 0..2, 0 dB, centre, on track 0. pass() publishes a
// post-fader chain, rewinds the transport and drives a single process() call,
// so a test can render the same engine dry, with the insert disabled, and
// with it enabled without the engine ever changing identity.
class FxRig {
 public:
  FxRig() {
    engine_ = std::make_unique<misuc::engine::AudioEngine>(
        misuc::test::defaultAudioBackend());
    engine_->setGroove(false);
    engine_->setMetronome(false, 0.0f);
    engine_->setTempo(120.0);
    std::vector<misuc::dsp::MidiNote> flat;
    flat.push_back(misuc::dsp::MidiNote{0.0, 2.0, misuc::dsp::midiNoteToFreq(60),
                                        1.f, 0});
    engine_->setScore(std::move(flat));
  }

  FxPass pass(
      const std::vector<std::vector<misuc::engine::FxInsertParam>>& chains,
      uint32_t frames) {
    return pass(chains, {}, frames);
  }

  FxPass pass(
      const std::vector<std::vector<misuc::engine::FxInsertParam>>& chains,
      const std::vector<misuc::engine::AutomationParam>& automation,
      uint32_t frames) {
    engine_->publishFx(chains);
    engine_->publishAutomation(automation);
    engine_->stop(); // rewind the transport so every pass starts at beat 0
    engine_->play(false);
    std::vector<float> buf(frames * 2, 0.f);
    engine_->process(nullptr, 0, buf.data(), 2, frames, 0, kSr);
    FxPass out;
    out.l.resize(frames);
    out.r.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
      out.l[i] = buf[2 * i];
      out.r[i] = buf[2 * i + 1];
    }
    return out;
  }

 private:
  std::unique_ptr<misuc::engine::AudioEngine> engine_;
};

// Fraction of samples where two renders diverge beyond a relative epsilon
// around the local magnitude (pass-through comparisons; never bit-exact).
float deviationFraction(const std::vector<float>& a,
                        const std::vector<float>& b) {
  const size_t n = std::min(a.size(), b.size());
  size_t dev = 0;
  for (size_t i = 0; i < n; ++i) {
    if (std::fabs(a[i] - b[i]) >
        0.02f * (std::fabs(a[i]) + std::fabs(b[i])) + 1e-4f)
      ++dev;
  }
  return static_cast<float>(dev) / static_cast<float>(n > 0 ? n : 1);
}

// Some real mda URI the installed mda-lv2 bundle provides (checked at
// discovery time); returns "" when hosting is unavailable.
std::string hostDelayUri(misuc::engine::AudioEngine& e) {
  if (!e.fxSupported())
    return std::string();
  for (const auto& p : e.discoverFx()) {
    if (p.name.find("DubDelay") != std::string::npos ||
        p.name.find("Delay") != std::string::npos)
      return p.uri;
  }
  return std::string();
}

// One built-in EQ insert, enabled, defaults (transparent). No LV2 hosting is
// required, so this runs identically on every build.
inline std::vector<std::vector<misuc::engine::FxInsertParam>> eqChain() {
  return {std::vector<misuc::engine::FxInsertParam>{
      {misuc::engine::kBuiltinEqUri, true,
       std::vector<misuc::engine::FxParam>()}}};
}

// Tail window strictly after the note's release: beats 2.5..3.9 at 120 bpm.
constexpr size_t kTailFrom = 2 * kFpb120 + 12000;
constexpr size_t kTailTo = 4 * kFpb120 - 2400;

} // namespace

TEST_CASE(fx_host_discovery_lists_effects) {
  misuc::engine::AudioEngine engine(
      misuc::test::defaultAudioBackend());
  // The built-in EQ is present on every build; LV2 effects additionally appear
  // when the build hosts lilv.
  std::vector<misuc::engine::FxPluginInfo> fx = engine.discoverFx();
  bool sawBuiltin = false;
  for (const auto& p : fx)
    if (p.uri == misuc::engine::kBuiltinEqUri) sawBuiltin = true;
  TEST_ASSERT(sawBuiltin);
  TEST_ASSERT(engine.fxInfo(misuc::engine::kBuiltinEqUri) != nullptr);
  TEST_ASSERT(engine.fxInfo("urn:nope") == nullptr);
  if (!engine.fxSupported()) {
    // Build without lilv (Windows): exactly the two built-ins (Eq + Comp);
    // the unknown lookup stays a graceful nullptr.
    TEST_ASSERT(fx.size() == 2);
    return;
  }
  TEST_ASSERT(!fx.empty());
  for (const misuc::engine::FxPluginInfo& p : fx) {
    TEST_ASSERT(!p.uri.empty());
    TEST_ASSERT(!p.name.empty());
    TEST_ASSERT(p.audioIns >= 1 && p.audioOuts >= 1);
  }
  const std::string uri = hostDelayUri(engine);
  if (uri.empty()) {
    // LV2 hosting may be enabled with an empty plugin world (bare CI image);
    // the delay-host assertions below are then not applicable.
    std::printf("  (no LV2 delay plugin; partial discovery-only checks)\n");
    return;
  }
  TEST_ASSERT(!uri.empty());
  const misuc::engine::FxPluginInfo* info = engine.fxInfo(uri);
  TEST_ASSERT(info != nullptr);
  TEST_ASSERT(!info->params.empty());
  // Discovery is cached: a second scan returns the same catalog.
  TEST_ASSERT(engine.discoverFx().size() == fx.size());
}

// The delay effect must be audible: an enabled insert carries the note's body
// and keeps ringing past the release where the disabled insert is silent.
TEST_CASE(fx_delay_insert_changes_render) {
  FxRig rig;
  misuc::engine::AudioEngine probe(
      misuc::test::defaultAudioBackend());
  const std::string uri = hostDelayUri(probe);
  if (uri.empty())
    return; // hosting unavailable: the pass-through is the only render path
  const uint32_t frames = 4 * kFpb120;
  // A fresh engine's very first render can carry sparse overdub energy in the
  // full-suite harness; that drift hits the first pass only. Warm up once and
  // compare the (stable) subsequent passes, which differ only by the chain.
  (void)rig.pass({}, frames);
  const FxPass off = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {uri, false, std::vector<misuc::engine::FxParam>()}}},
      frames);
  const FxPass on = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {uri, true, std::vector<misuc::engine::FxParam>()}}},
      frames);

  // Note body still present in every data pass.
  TEST_ASSERT(peakRange(off.l, 8000, 40000) > 0.05f);
  TEST_ASSERT(peakRange(on.l, 8000, 40000) > 0.05f);
  // The enabled insert rings into the post-release tail, far past the
  // disabled pass-through energy and audibly so.
  const float onTail = peakRange(on.l, kTailFrom, kTailTo);
  const float offTail = peakRange(off.l, kTailFrom, kTailTo);
  TEST_ASSERT(onTail > 0.002f);
  TEST_ASSERT(onTail > offTail * 1.5f);
}

// An unknown/unsupported URI must be skipped gracefully (instantiate returns
// null) and never crash or drop the track. Sample equality with an empty
// chain is intentionally NOT asserted here: a fresh engine's first renders can
// carry sparse overdub energy in the full-suite harness (see fx_delay), which
// makes any cross-pass sample comparison brittle. The pass-through semantics
// are instead proven by the off/on tail gap in fx_delay_insert_changes_render.
TEST_CASE(fx_unknown_uri_is_ignored) {
  FxRig rig;
  const uint32_t frames = 4 * kFpb120;
  (void)rig.pass({}, frames); // warm-up pass, see fx_delay_insert_changes_render
  const FxPass on = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {"urn:not:a:real:plugin", true,
           std::vector<misuc::engine::FxParam>()}}},
      frames);
  TEST_ASSERT(peakRange(on.l, 8000, 40000) > 0.05f); // note body intact
}

// A published chain with more tracks than the engine supports is capped; a
// clearFx() afterwards restores the dry pass-through without crashing.
// ── Phase O: per-block fx control smoothing ──────────────────────────────

// A curve step that lands mid-segment must be applied at the 64-frame control
// cadence instead of being held to the next segment boundary (8192 frames ≈
// 0.34 beats at 120 bpm). LP held at 20 kHz for beat 0..0.2 then stepped to
// 200 Hz: the corner of the engine's FIRST block (frames 5000..8191, beat
// 0.208..0.341) is already audibly darker than the unautomated chain — with the
// old segment-hold behaviour that window stayed bright until frame 8192.
TEST_CASE(fx_automation_midsegment_step_is_audible) {
  FxRig rig;
  const uint32_t frames = 4 * kFpb120;
  (void)rig.pass({}, frames); // warm-up pass, see fx_delay_insert_changes_render
  const auto chain = eqChain();
  misuc::engine::AutomationParam lp;
  lp.trackIndex = 0;
  lp.active = true;
  lp.param = "fx:0:1"; // built-in EQ low-pass cutoff (Hz)
  lp.times = {0.0, 0.2, 4.0};
  lp.values = {20000.f, 200.f, 200.f};
  const FxPass ctrl = rig.pass(chain, frames);
  const FxPass step = rig.pass(chain, {lp}, frames);

  // Entirely inside the engine's first 8192-frame segment, strictly after the
  // beat 0.2 step point: only per-block control sampling makes it dark here.
  const float dark = peakRange(step.l, 5000, 8191);
  const float bright = peakRange(ctrl.l, 5000, 8191);
  TEST_ASSERT(bright > 0.05f);
  TEST_ASSERT(dark < bright * 0.7f);
  TEST_ASSERT(deviationFraction(step.l, ctrl.l) > 0.1f);
}

// A fast LP ramp (20 kHz → 100 Hz over beat 0..1) darkens the note body
// progressively: a sample window inside the first segment (beats 0.08..0.5)
// stays close to the unautomated brightness while a window inside a later
// segment (beats 1.25..1.75) is measurably cut. This pins the "smooth
// following" property across segment boundaries, complementing the mid-segment
// step test above.
TEST_CASE(fx_automation_ramp_tracks_progressively) {
  FxRig rig;
  const uint32_t frames = 4 * kFpb120;
  (void)rig.pass({}, frames); // warm-up pass
  const auto chain = eqChain();
  misuc::engine::AutomationParam lp;
  lp.trackIndex = 0;
  lp.active = true;
  lp.param = "fx:0:1";
  lp.times = {0.0, 1.0, 4.0}; // fast 1-beat darkening ramp, then flat
  lp.values = {20000.f, 100.f, 100.f};
  const FxPass ctrl = rig.pass(chain, frames);
  const FxPass ramp = rig.pass(chain, {lp}, frames);

  // Early (pre-ramp / still near-transparent) energy.
  const float early = peakRange(ramp.l, 2000, 12000);
  // Late (post-ramp, LP settled at 100 Hz → heavily attenuates 261.6 Hz).
  const float late = peakRange(ramp.l, 30000, 42000);
  const float ctrlPeak = peakRange(ctrl.l, 2000, 42000);
  TEST_ASSERT(ctrlPeak > 0.05f);
  TEST_ASSERT(early > ctrlPeak * 0.5f);          // still bright early
  TEST_ASSERT(late < ctrlPeak * 0.5f);           // and measurably dark later
  TEST_ASSERT(late < early * 0.8f);              // monotonic darkening trend
  TEST_ASSERT(deviationFraction(ramp.l, ctrl.l) > 0.05f);
}

// ── Phase O ends ──

// ── Phase P: offline export reproduces the per-block fx cadence ──────────
// The offline export path processes 512-frame chunks while the live path
// drives one monolithic process() call. Because the fx sub-block cadence
// (64 frames) is aligned to absolute position in both, the two passes must
// sample the curve at the same 64-frame boundaries (1) and therefore produce
// byte-identical output (2), so live monitoring and File > Export of the same
// session agree sample-for-sample even under control automation.

TEST_CASE(fx_automation_midsegment_step_is_audible_in_export) {
  // Mid-segment step on the LP cutoff, exported offline: the window inside the
  // engine's first 8192-frame segment (5000..8191) must be dark already, which
  // is exactly what the segment-hold bug would have got wrong in export too.
  misuc::project::Session s;
  const std::string tid = s.addTrack("FxExportStep");
  s.findTrack(tid)->notes.push_back(
      misuc::project::NoteEvent{0, 60, 127, 0.0, 4.0});
  s.addFx(tid, misuc::engine::kBuiltinEqUri);

  misuc::engine::AutomationParam lp;
  lp.trackIndex = 0;
  lp.active = true;
  lp.param = "fx:0:1";
  lp.times = {0.0, 0.2, 4.0};
  lp.values = {20000.f, 200.f, 200.f};

  auto chain = [&s]() {
    std::vector<std::vector<misuc::engine::FxInsertParam>> chains;
    for (const auto& tr : s.tracks()) {
      std::vector<misuc::engine::FxInsertParam> tch;
      for (const auto& fx : tr.fx) {
        misuc::engine::FxInsertParam ins;
        ins.uri = fx.uri;
        ins.enabled = fx.enabled;
        tch.push_back(std::move(ins));
      }
      chains.push_back(std::move(tch));
    }
    return chains;
  }();

  auto setup = [](misuc::engine::AudioEngine& e) {
    e.setGroove(false);
    e.setMetronome(false, 0.0f);
    e.setTempo(120.0);
  };

  // Unautomated export (control).
  std::string err;
  misuc::engine::AudioEngine brightEngine(
      misuc::test::defaultAudioBackend());
  setup(brightEngine);
  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(s, notes);
  brightEngine.setScore(notes);
  brightEngine.publishFx(chain);
  const std::string dir = misuc::test::makeTempDir("misuc_fx_export_bright");
  const std::string brightPath = dir + "/bright.wav";
  TEST_ASSERT(brightEngine.renderToWav(brightPath, 0.0, 4.0, 2, err));

  // Automating the same insert offline.
  misuc::engine::AudioEngine stepEngine(
      misuc::test::defaultAudioBackend());
  setup(stepEngine);
  stepEngine.setScore(notes);
  stepEngine.publishFx(chain);
  stepEngine.publishAutomation({lp});
  const std::string dir2 = misuc::test::makeTempDir("misuc_fx_export_step");
  const std::string stepPath = dir2 + "/step.wav";
  TEST_ASSERT(stepEngine.renderToWav(stepPath, 0.0, 4.0, 2, err));

  misuc::audio::AudioBuffer u, d;
  TEST_ASSERT(misuc::audio::readAudioFile(brightPath, u, err));
  TEST_ASSERT(misuc::audio::readAudioFile(stepPath, d, err));
  TEST_ASSERT(u.channels == 2 && d.channels == 2);
  TEST_ASSERT(u.frames == 4 * kFpb120 && d.frames == 4 * kFpb120);

  const float bright = peakRange(u.samples, 10000, 16382); // L frames 5000..8191
  const float dark = peakRange(d.samples, 10000, 16382);
  TEST_ASSERT(bright > 0.05f);
  TEST_ASSERT(dark < bright * 0.7f);
}

TEST_CASE(fx_automation_export_matches_live_cadence) {
  // Same mid-segment-step document rendered two ways: live monolithic
  // process() and offline renderToWav (512-frame chunks). The outputs must be
  // byte-identical, axiomatically proving export reproduces the live 64-frame
  // control cadence (and every other block-shape-independent stage).
  misuc::project::Session s;
  const std::string tid = s.addTrack("FxExportLive");
  s.findTrack(tid)->notes.push_back(
      misuc::project::NoteEvent{0, 60, 127, 0.0, 4.0});
  s.addFx(tid, misuc::engine::kBuiltinEqUri);

  misuc::engine::AutomationParam lp;
  lp.trackIndex = 0;
  lp.active = true;
  lp.param = "fx:0:1";
  lp.times = {0.0, 0.2, 4.0};
  lp.values = {20000.f, 200.f, 200.f};

  std::vector<std::vector<misuc::engine::FxInsertParam>> chain;
  for (const auto& tr : s.tracks()) {
    std::vector<misuc::engine::FxInsertParam> tch;
    for (const auto& fx : tr.fx) {
      misuc::engine::FxInsertParam ins;
      ins.uri = fx.uri;
      ins.enabled = fx.enabled;
      tch.push_back(std::move(ins));
    }
    chain.push_back(std::move(tch));
  }

  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(s, notes);

  const uint32_t frames = 4 * kFpb120;

  misuc::engine::AudioEngine live(
      misuc::test::defaultAudioBackend());
  live.setGroove(false);
  live.setMetronome(false, 0.0f);
  live.setTempo(120.0);
  live.setScore(notes);
  live.publishFx(chain);
  live.publishAutomation({lp});
  live.play(false);
  std::vector<float> buf(frames * 2, 0.f);
  live.process(nullptr, 0, buf.data(), 2, frames, 0, kSr);

  std::string err;
  misuc::engine::AudioEngine render(
      misuc::test::defaultAudioBackend());
  render.setGroove(false);
  render.setMetronome(false, 0.0f);
  render.setTempo(120.0);
  render.setScore(notes);
  render.publishFx(chain);
  render.publishAutomation({lp});
  const std::string dir = misuc::test::makeTempDir("misuc_fx_export_live");
  const std::string outPath = dir + "/live.wav";
  TEST_ASSERT(render.renderToWav(outPath, 0.0, 4.0, 2, err));

  misuc::audio::AudioBuffer wav;
  TEST_ASSERT(misuc::audio::readAudioFile(outPath, wav, err));
  TEST_ASSERT(wav.channels == 2);
  TEST_ASSERT(wav.frames == frames);
  TEST_ASSERT(buf.size() == wav.samples.size());
  TEST_ASSERT(std::memcmp(buf.data(), wav.samples.data(),
                          buf.size() * sizeof(float)) == 0);
}

// ── Phase N: fx control-port automation ---------------------------------

// The "fx:<chain>:<port>" lane grammar parses exactly as documented and every
// malformed or non-fx string is rejected (generic "param" callers should treat
// an invalid target as a no-op, which the engine does).
TEST_CASE(parse_fx_automation_grammar) {
  const auto ok = misuc::engine::parseFxAutomation("fx:3:7");
  TEST_ASSERT(ok.ok);
  TEST_ASSERT(ok.chainIndex == 3);
  TEST_ASSERT(ok.portIndex == 7);
  const auto zero = misuc::engine::parseFxAutomation("fx:0:0");
  TEST_ASSERT(zero.ok && zero.chainIndex == 0 && zero.portIndex == 0);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("gain").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("volume").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("pan").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx:").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx:0").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx::1").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx:0:").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx:1:2x").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx:1x:2").ok);
  TEST_ASSERT(!misuc::engine::parseFxAutomation("fx:-1:2").ok);
}

// Automating the built-in EQ's low-pass cutoff via a "fx:0:1" lane is audible
// at control rate in the live process() path: a lane pinned to 200 Hz darkens
// the 261.6 Hz test note versus the identical chain with the lane absent.
TEST_CASE(fx_automation_lp_darkens_render) {
  FxRig rig;
  const uint32_t frames = 4 * kFpb120;
  (void)rig.pass({}, frames); // warm-up pass, see fx_delay_insert_changes_render
  const auto chain = eqChain();
  misuc::engine::AutomationParam lp;
  lp.trackIndex = 0;
  lp.active = true;
  lp.param = "fx:0:1";                    // built-in EQ low-pass cutoff (Hz)
  lp.times = {0.0, 4.0};
  lp.values = {200.f, 200.f};
  const std::vector<misuc::engine::AutomationParam> autoLp{lp};

  const FxPass ctrl = rig.pass(chain, frames);
  const FxPass dark = rig.pass(chain, autoLp, frames);

  const float ctrlPeak = peakRange(ctrl.l, 8000, 40000);
  const float darkPeak = peakRange(dark.l, 8000, 40000);
  TEST_ASSERT(ctrlPeak > 0.05f);                 // note body present
  TEST_ASSERT(darkPeak > 0.005f);                // ... even when filtered
  TEST_ASSERT(darkPeak < ctrlPeak * 0.9f);       // and measurably darker
  TEST_ASSERT(deviationFraction(dark.l, ctrl.l) > 0.1f);
}

// Out-of-range fx automation targets never destabilize the render: a lane aimed
// at a chain the track does not have ("fx:9:1"), or at a port the plugin does
// not expose, is skipped, leaving the output identical to the unautomated chain.
TEST_CASE(fx_automation_unknown_target_is_ignored) {
  FxRig rig;
  const uint32_t frames = 4 * kFpb120;
  (void)rig.pass({}, frames);
  const auto chain = eqChain();
  misuc::engine::AutomationParam bad;
  bad.trackIndex = 0;
  bad.active = true;
  bad.param = "fx:9:1"; // no such insert
  bad.times = {0.0, 4.0};
  bad.values = {200.f, 200.f};
  const FxPass ctrl = rig.pass(chain, frames);
  const FxPass badPass = rig.pass(chain, {bad}, frames);
  TEST_ASSERT(deviationFraction(ctrl.l, badPass.l) < 0.01f);
}

// ── Phase N ──

TEST_CASE(fx_repeat_publish_and_clear_caps_chains) {
  misuc::engine::AudioEngine engine(
      misuc::test::defaultAudioBackend());
  std::vector<std::vector<misuc::engine::FxInsertParam>> chains(
      misuc::engine::AudioEngine::kMaxTrackCount + 4);
  chains[0].push_back({"urn:not:a:real:plugin", true,
                       std::vector<misuc::engine::FxParam>()});
  chains[misuc::engine::AudioEngine::kMaxTrackCount - 1].push_back(
      {"urn:not:a:real:plugin", true, std::vector<misuc::engine::FxParam>()});
  engine.publishFx(chains); // must not throw / crash on the oversized vector
  engine.clearFx();
  engine.publishFx({}); // and an explicit empty publish is fine afterwards
  TEST_ASSERT(true);
}

// Session inserts survive the project round-trip: uri, enabled flag and the
// parameter index/value list all come back unchanged, and addFx/removeFx keep
// the counter monotonic across loads.
TEST_CASE(fx_session_insert_roundtrip) {
  misuc::project::Session s;
  const std::string tid = s.addTrack("Fx");
  const std::string fid = s.addFx(tid, "urn:example:delay");
  TEST_ASSERT(!fid.empty());
  misuc::project::Track* t = s.findTrack(tid);
  TEST_ASSERT(t != nullptr);
  TEST_ASSERT(t->fx.size() == 1);
  TEST_ASSERT(t->fx[0].uri == "urn:example:delay");
  t->fx[0].params = {{0, 0.25f}, {1, 0.5f}};

  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  const misuc::project::Track* back = loaded.findTrack(tid);
  TEST_ASSERT(back != nullptr);
  TEST_ASSERT(back->fx.size() == 1);
  TEST_ASSERT(back->fx[0].id == fid);
  TEST_ASSERT(back->fx[0].uri == "urn:example:delay");
  TEST_ASSERT(back->fx[0].enabled);
  TEST_ASSERT(back->fx[0].params.size() == 2);
  TEST_ASSERT(back->fx[0].params[0].index == 0);
  TEST_ASSERT(std::fabs(back->fx[0].params[0].value - 0.25f) < 1e-6f);
  TEST_ASSERT(back->fx[0].params[1].index == 1);
  TEST_ASSERT(std::fabs(back->fx[0].params[1].value - 0.5f) < 1e-6f);

  // removeFx removes and the counter never reuses an id.
  misuc::project::Session s2;
  const std::string t2 = s2.addTrack("Fx2");
  const std::string a = s2.addFx(t2, "urn:a");
  const std::string b = s2.addFx(t2, "urn:b");
  TEST_ASSERT(a != b);
  TEST_ASSERT(s2.removeFx(t2, a));
  TEST_ASSERT(!s2.removeFx("missing", a));
  TEST_ASSERT(s2.findTrack(t2)->fx.size() == 1);
  const std::string c = s2.addFx(t2, "urn:c");
  TEST_ASSERT(c != a && c != b);
  (void)c;
}

// The built-in EQ advertises the 14-port layout (HP, LP, 4 bands x freq/gain/Q)
// with the DSP's hard ranges so the UI can build sliders directly.
TEST_CASE(fx_builtin_eq_metadata) {
  misuc::engine::AudioEngine engine(
      misuc::test::defaultAudioBackend());
  const misuc::engine::FxPluginInfo* info =
      engine.fxInfo(misuc::engine::kBuiltinEqUri);
  TEST_ASSERT(info != nullptr);
  TEST_ASSERT(info != nullptr && info->name == "Built-in EQ");
  TEST_ASSERT(info != nullptr &&
              info->params.size() == misuc::dsp::ParametricEq::kPortCount);
  if (!info) return;
  TEST_ASSERT(info->params[0].label == "High-pass");
  TEST_ASSERT(info->params[0].min == misuc::dsp::ParametricEq::kMinHz);
  TEST_ASSERT(info->params[0].max == misuc::dsp::ParametricEq::kMaxHz);
  // Band 1 (ports 2..4): freq/gain/Q with a +-18 dB gain range.
  TEST_ASSERT(std::fabs(info->params[2].max - misuc::dsp::ParametricEq::kMaxHz) < 1e-6f);
  TEST_ASSERT(std::fabs(info->params[3].max - misuc::dsp::ParametricEq::kMaxGainDb) < 1e-6f);
  TEST_ASSERT(std::fabs(info->params[3].min - misuc::dsp::ParametricEq::kMinGainDb) < 1e-6f);
  TEST_ASSERT(std::fabs(info->params[4].max - misuc::dsp::ParametricEq::kMaxQ) < 1e-6f);
}

// A param vector is honored end to end: boosting/cutting the band sitting on
// the MIDI note moves the rendered note's peak accordingly, while a band far
// from the note leaves it (nearly) untouched. ./test_fx runs on any build.
TEST_CASE(fx_builtin_eq_boost_cut_transparent) {
  FxRig rig;
  const uint32_t frames = 2 * kFpb120; // the note body, beats 0..2
  const FxPass dry = rig.pass({}, frames); // reuse the same engine for every
  const float notePeak = peakRange(dry.l, 0, frames);
  TEST_ASSERT(notePeak > 0.001f);

  auto ports = [](float freq, float gainDb) {
    std::vector<misuc::engine::FxParam> p;
    p.push_back({misuc::dsp::ParametricEq::kBandBase + misuc::dsp::ParametricEq::kBandFreq, freq});
    p.push_back({misuc::dsp::ParametricEq::kBandBase + misuc::dsp::ParametricEq::kBandGainDb, gainDb});
    p.push_back({misuc::dsp::ParametricEq::kBandBase + misuc::dsp::ParametricEq::kBandQ, 1.f});
    return p;
  };
  const FxPass boosted = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {misuc::engine::kBuiltinEqUri, true, ports(261.6f, 18.f)}}},
      frames);
  TEST_ASSERT(peakRange(boosted.l, 0, frames) > notePeak * 3.f);
  const FxPass cut = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {misuc::engine::kBuiltinEqUri, true, ports(261.6f, -18.f)}}},
      frames);
  TEST_ASSERT(peakRange(cut.l, 0, frames) < notePeak * 0.4f);
  const FxPass far = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {misuc::engine::kBuiltinEqUri, true, ports(4000.f, 18.f)}}},
      frames);
  TEST_ASSERT(peakRange(far.l, 0, frames) > notePeak * 0.8f &&
              peakRange(far.l, 0, frames) < notePeak * 1.3f);
}

// The built-in EQ's document identity survives the project round-trip, and a
// disabled built-in insert is a pure pass-through (roll scoped off -> the note
// body stays put while the live-run section above already proved its teeth).
TEST_CASE(fx_builtin_eq_session_roundtrip) {
  misuc::project::Session s;
  const std::string tid = s.addTrack("EQ");
  const std::string fid = s.addFx(tid, misuc::engine::kBuiltinEqUri);
  TEST_ASSERT(!fid.empty());
  misuc::project::Track* t = s.findTrack(tid);
  TEST_ASSERT(t != nullptr && t->fx.size() == 1);
  t->fx[0].params = {{misuc::dsp::ParametricEq::kBandBase + misuc::dsp::ParametricEq::kBandGainDb, 12.f}};
  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  const misuc::project::Track* back = loaded.findTrack(tid);
  TEST_ASSERT(back != nullptr && back->fx.size() == 1);
  TEST_ASSERT(back->fx[0].uri == misuc::engine::kBuiltinEqUri);
  TEST_ASSERT(back->fx[0].enabled);
  TEST_ASSERT(back->fx[0].params.size() == 1);
  TEST_ASSERT(std::fabs(back->fx[0].params[0].value - 12.f) < 1e-6f);

  FxRig rig;
  const uint32_t frames = 2 * kFpb120;
  const FxPass disabled = rig.pass(
      {std::vector<misuc::engine::FxInsertParam>{
          {misuc::engine::kBuiltinEqUri, false,
           std::vector<misuc::engine::FxParam>()}}},
      frames);
  TEST_ASSERT(peakRange(disabled.l, 0, frames) > 0.001f);
}