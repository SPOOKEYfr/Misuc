// Headroom / clipping diagnostic for the Misuc master path.
//
// Usage:
//   misuc_headroom <project.misuc>   render the project's full arrangement,
//                                    report true peak / RMS / clipped-sample
//                                    stats on the offline export
//   misuc_headroom stress <tracks>[ <masterDb>]   N hot MIDI channels at unity
//                                    summed through the master; report the
//                                    peak / clipped stats vs track count
//   misuc_headroom worstcase         Phase F worst-case routing: 125 hot
//                                    inserts round-tripped through toJson/
//                                    fromJson and routed across all 32 buses to
//                                    master; verify every insert + bus meter
//                                    carried audio and the limiter held every
//                                    sample under the hard-clip ceiling
//
// Measurement harness for the "no master clipping" acceptance check. It drives
// the exact user-heard path (AudioEngine::renderToWav == live process()) and
// publishes sessions exactly as the UI does (mixer gains, buses, automation,
// master gain). Also exposes the synth+sum headroom so we can see at which
// point multi-track summing crosses 0 dBFS.

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/ClipScore.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/project/Session.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

void report(const std::string& label, const misuc::audio::AudioBuffer& w,
            double masterDb) {
  uint64_t over999 = 0, run = 0, maxRun = 0;
  double peak = 0.0, acc = 0.0;
  double peakDb = -200.0;
  for (float v : w.samples) {
    double a = std::fabs(static_cast<double>(v));
    peak = std::max(peak, a);
    acc += a * a;
    if (a > 0.999) {
      ++over999;
      ++run;
      maxRun = std::max(maxRun, run);
    } else {
      run = 0;
    }
  }
  if (peak > 1e-12) peakDb = 20.0 * std::log10(peak);
  double rms =
      std::sqrt(acc / static_cast<double>(w.samples.empty() ? 1 : w.samples.size()));
  printf("%-22s channels=%d frames=%llu  peak=%.4f (%.1f dBFS)  "
         "rms=%.4f (%.1f dBFS)  |x|>0.999 samples=%llu  maxRun=%llu  "
         "master=%.1f dB  %s\n",
         label.c_str(), static_cast<int>(w.channels),
         static_cast<unsigned long long>(w.frames), peak, peakDb, rms,
         20.0 * std::log10(rms > 1e-12 ? rms : 1e-12),
         static_cast<unsigned long long>(over999),
         static_cast<unsigned long long>(maxRun), masterDb,
         (over999 ? "  <-- CLIPPED" : ""));
}

void publishFromSession(misuc::engine::AudioEngine& engine,
                        const misuc::project::Session& s) {
  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(s, notes);
  engine.setScore(std::move(notes));

  std::vector<misuc::dsp::PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  misuc::engine::buildClipScore(s, clips, storage, err);
  engine.setClips(std::move(clips), std::move(storage));

  std::vector<misuc::engine::MixerTrackParam> tracks;
  std::vector<misuc::engine::MixerBusParam> buses;
  std::map<std::string, int> busIdx;
  for (size_t i = 0; i < s.buses().size(); ++i) busIdx[s.buses()[i].id] = static_cast<int>(i);
  for (const auto& b : s.buses()) {
    misuc::engine::MixerBusParam p;
    p.gainDb = b.faderDb;
    p.pan = b.pan;
    p.mute = b.muted;
    p.solo = b.solo;
    buses.push_back(p);
  }
  for (const auto& t : s.tracks()) {
    misuc::engine::MixerTrackParam p;
    p.gainDb = t.gainDb;
    p.pan = t.pan;
    p.mute = t.muted;
    p.solo = t.solo;
    auto it = busIdx.find(t.busId);
    p.busIndex = (t.busId.empty() || it == busIdx.end()) ? -1 : it->second;
    tracks.push_back(p);
  }
  engine.publishMixer(tracks, buses);

  std::vector<misuc::engine::AutomationParam> autos;
  uint32_t ti = 0;
  for (const auto& t : s.tracks()) {
    for (const auto& lane : t.automation) {
      if (lane.param != "gain") continue;
      misuc::engine::AutomationParam a;
      a.trackIndex = ti;
      a.active = !lane.muted && !lane.points.empty();
      for (const auto& p : lane.points) {
        a.times.push_back(p.timeBeats);
        a.values.push_back(static_cast<float>(p.value));
      }
      autos.push_back(std::move(a));
    }
    ++ti;
  }
  engine.publishAutomation(std::move(autos));
}

int runProject(const std::string& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", file.c_str());
    return 2;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  auto s = misuc::project::Session::fromJson(ss.str());

  auto engine = std::make_unique<misuc::engine::AudioEngine>(
      std::make_unique<misuc::engine::PipeWireBackend>());
  engine->setTempo(s.tempo());
  engine->setMetronome(false, 0.0f);
  engine->setGroove(false);
  engine->setTestTone(false);
  engine->setMasterGainDb(s.masterGainDb());
  engine->setLoop(s.loopMode(), s.loopStart(), s.loopEnd());
  publishFromSession(*engine, s);

  double fpb = engine->transport().framesPerBeat();
  double totalBeats = s.contentEndBeats();
  if (!(totalBeats > 0.0)) totalBeats = 4.0 * 32.0 * s.timeSigNumerator() / s.timeSigDenominator();
  printf("project: '%s'  bpm=%.1f  tracks=%zu buses=%zu  contentEnd=%.1f beats "
         "(=%.0f frames)  master=%.1f dB\n",
         s.name().c_str(), s.tempo(), s.tracks().size(), s.buses().size(),
         totalBeats, totalBeats * fpb, s.masterGainDb());
  std::printf(
      "  per-track mixer gainDb: ");
  for (const auto& t : s.tracks())
    std::printf("%.1f ", t.gainDb);
  std::printf("\n");

  std::string wav = "/tmp/misuc_headroom_proj.wav";
  std::string err;
  engine->play(false); // start the transport the way the UI does
  if (!engine->renderToWav(wav, 0.0, totalBeats, 2, err)) {
    std::fprintf(stderr, "render failed: %s\n", err.c_str());
    return 3;
  }
  misuc::audio::AudioBuffer out;
  if (!misuc::audio::readAudioFile(wav, out, err)) {
    std::fprintf(stderr, "read failed: %s\n", err.c_str());
    return 4;
  }
  report("full arrangement", out, s.masterGainDb());
  return 0;
}

int runStress(int nTracks, double masterDb) {
  if (nTracks < 1) nTracks = 1;
  if (nTracks > 32) nTracks = 32;
  for (int n = 1; n <= nTracks; ++n) {
    std::vector<misuc::dsp::MidiNote> notes;
    // Hammer A1 bass + A2 thump on every beat at full velocity, on `n` tracks.
    for (int t = 0; t < n; ++t) {
      for (int b = 0; b < 4; ++b) {
        misuc::dsp::MidiNote a, a2;
        a.startBeats = b; a.durationBeats = 1.0;
        a.freq = 55.0; a.gain = 1.0f; a.track = static_cast<uint32_t>(t);
        a2.startBeats = b; a2.durationBeats = 1.0;
        a2.freq = 110.0; a2.gain = 1.0f; a2.track = static_cast<uint32_t>(t);
        notes.push_back(a);
        notes.push_back(a2);
      }
    }
    std::sort(notes.begin(), notes.end(),
              [](const misuc::dsp::MidiNote& x, const misuc::dsp::MidiNote& y) {
                return x.startBeats < y.startBeats;
              });
    auto engine = std::make_unique<misuc::engine::AudioEngine>(
        std::make_unique<misuc::engine::PipeWireBackend>());
    engine->setTempo(120.0);
    engine->setMetronome(false, 0.0f);
    engine->setGroove(false);
    engine->setMasterGainDb(static_cast<float>(masterDb));
    engine->setScore(std::move(notes));
    std::vector<misuc::engine::MixerTrackParam> tracks(n);
    for (auto& p : tracks) { p.gainDb = 0.0f; }
    engine->publishMixer(std::move(tracks));
    std::string wav = "/tmp/misuc_headroom_stress.wav";
    std::string err;
    engine->play(false);
    if (!engine->renderToWav(wav, 0.0, 4.0, 2, err)) {
      std::fprintf(stderr, "render failed: %s\n", err.c_str());
      return 3;
    }
    misuc::audio::AudioBuffer out;
    if (!misuc::audio::readAudioFile(wav, out, err)) return 4;
    char label[32];
    std::snprintf(label, sizeof label, "%d hot track%s", n, n == 1 ? "" : "s");
    report(label, out, masterDb);
  }
  return 0;
}

// Phase F DoD on the worst-case routing graph: a 125-insert session routed
// across every bus is round-tripped through toJson/fromJson, the RELOADED
// document drives the render, and every insert + bus is provably audible with
// no sample past the hard-clip ceiling (the master limiter holds -1 dBFS).
int runWorstCase() {
  constexpr uint32_t kInserts = 125;
  constexpr uint32_t kRoutes = 32; // AudioEngine::kMaxBusCount

  misuc::project::Session s;
  for (uint32_t b = 0; b < kRoutes; ++b)
    s.addBus("Bus " + std::to_string(b + 1));
  std::vector<std::vector<misuc::dsp::MidiNote>> notes(kInserts);
  for (uint32_t t = 0; t < kInserts; ++t) {
    const std::string tid = s.addTrack("Insert " + std::to_string(t + 1));
    if (misuc::project::Track* tr = s.findTrack(tid))
      tr->busId = s.buses()[t % kRoutes].id;
    // Hammer A1 bass + A2 thump on every beat at full velocity (unity gain).
    for (uint32_t b = 0; b < 4; ++b) {
      notes[t].push_back({static_cast<double>(b), 1.0, 55.0, 1.0f, t});
      notes[t].push_back({static_cast<double>(b), 1.0, 110.0, 1.0f, t});
    }
  }

  const misuc::project::Session loaded =
      misuc::project::Session::fromJson(s.toJson());
  if (loaded.tracks().size() != kInserts || loaded.buses().size() != kRoutes) {
    std::fprintf(stderr, "FAIL: round-trip lost inserts/buses "
                         "(%zu tracks / %zu buses)\n",
                 loaded.tracks().size(), loaded.buses().size());
    return 1;
  }
  for (uint32_t t = 0; t < kInserts; ++t) {
    const misuc::project::Track& back = loaded.tracks()[t];
    if (back.busId != s.buses()[t % kRoutes].id) {
      std::fprintf(stderr, "FAIL: insert %u lost its bus route\n", t);
      return 1;
    }
  }

  auto engine = std::make_unique<misuc::engine::AudioEngine>(
      std::make_unique<misuc::engine::PipeWireBackend>());
  engine->setTempo(120.0);
  engine->setMetronome(false, 0.0f);
  engine->setGroove(false);
  engine->setTestTone(false);
  engine->setMasterGainDb(0.f);
  publishFromSession(*engine, loaded); // mixer params + buses from the reload

  std::vector<misuc::dsp::MidiNote> flat;
  for (const auto& t : notes)
    for (const auto& n : t)
      flat.push_back(n);
  std::sort(flat.begin(), flat.end(),
            [](const misuc::dsp::MidiNote& x, const misuc::dsp::MidiNote& y) {
              return x.startBeats < y.startBeats;
            });
  engine->setScore(std::move(flat));

  constexpr uint32_t kFramesPerBeat = 24000u; // 120 bpm at 48 kHz
  constexpr uint32_t total = 4 * kFramesPerBeat;
  engine->play(false);
  std::vector<float> buf(total * 2, 0.f);
  engine->process(nullptr, 0, buf.data(), 2, total, 0, 48000);

  uint32_t audible = 0, silentBus = 0;
  for (uint32_t t = 0; t < kInserts; ++t)
    if (engine->trackMeterPeakL(t) > 1e-4f || engine->trackMeterPeakR(t) > 1e-4f)
      ++audible;
  for (uint32_t b = 0; b < kRoutes; ++b)
    if (!(engine->busMeterPeakL(b) > 1e-4f || engine->busMeterPeakR(b) > 1e-4f))
      ++silentBus;
  uint64_t hard = 0;
  double peak = 0.0;
  for (float v : buf) {
    const double a = std::fabs(static_cast<double>(v));
    peak = std::max(peak, a);
    if (a > 0.999) ++hard;
  }
  const bool ok = audible == kInserts && silentBus == 0 && hard == 0 &&
                  peak > 0.01;
  std::printf("worstcase routing: inserts=%u buses=%u audible=%u silentBuses=%u "
              "masterPeak=%.4f (%.1f dBFS) hardClipSamples=%llu  %s\n",
              kInserts, kRoutes, audible, silentBus, peak,
              peak > 1e-12 ? 20.0 * std::log10(peak) : -200.0,
              static_cast<unsigned long long>(hard),
              (ok ? "PASS" : "FAIL"));
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: misuc_headroom <project.misuc>\n"
                 "       misuc_headroom stress <tracks> [masterDb]\n"
                 "       misuc_headroom worstcase\n");
    return 1;
  }
  if (std::string(argv[1]) == "stress") {
    int n = argc >= 3 ? std::atoi(argv[2]) : 8;
    double db = argc >= 4 ? std::atof(argv[3]) : 0.0;
    return runStress(n, db);
  }
  if (std::string(argv[1]) == "worstcase")
    return runWorstCase();
  return runProject(argv[1]);
}