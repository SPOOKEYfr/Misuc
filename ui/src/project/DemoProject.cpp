#include "project/DemoProject.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/Json.h>
#include <misuc/engine/project/Session.h>

namespace misuc::ui {

namespace {
const double kTempo = 120.0;
const double kSrate = 48000.0;
const double kPi = 3.14159265358979323846;

void note(misuc::project::Track& t, int pitch, double start, double dur,
          int vel = 100) {
  misuc::project::NoteEvent n;
  n.pitch = static_cast<uint8_t>(pitch);
  n.velocity = static_cast<uint8_t>(vel);
  n.startBeats = start;
  n.durationBeats = dur;
  t.notes.push_back(n);
}

// Two-bar drum groove (beats 0..8), repeated later by the caller.
void drumGroove(misuc::project::Track& drums, int bar) {
  const double b = bar * 4.0;
  for (int i = 0; i < 4; ++i)
    note(drums, 36, b + i, 0.9, 112);              // kick on quarters
  note(drums, 38, b + 1, 0.55, 108);               // snare backbeat
  note(drums, 38, b + 3, 0.55, 108);
  for (int i = 0; i < 8; ++i)
    note(drums, 42, b + i * 0.5, 0.24, 88);        // closed hats on eighths
}

// i – vi – IV – V / IV – iii – ii – I: one chord per bar, Cmaj73 voicings.
void chordBass(misuc::project::Track& keys, misuc::project::Track& bass,
               int bar) {
  const int idx = (bar / 2) % 8;
  const int chords[8][4] = {           // roots: C A F G / F E D C
      {60, 64, 67, 71}, {57, 60, 64, 67}, {57, 60, 64, 69}, {59, 62, 65, 67},
      {57, 60, 64, 69}, {56, 59, 62, 67}, {50, 57, 62, 65}, {48, 55, 60, 64}};
  const int roots[8] = {48, 45, 41, 43, 41, 40, 38, 36};
  const double b = bar * 4.0;
  const int* chord = chords[idx];
  for (int i = 0; i < 4; ++i)
    note(keys, chord[i], b, 3.6, 74);   // one chord per bar
  note(bass, roots[idx], b, 3.5, 100);
  note(bass, roots[idx] + 12, b + 2, 1.4, 78);
}

std::vector<float> makePadWav(double seconds) {
  const double sr = kSrate;
  const size_t frames = static_cast<size_t>(seconds * sr);
  const double freqs[4] = {261.63, 329.63, 392.00, 493.88};
  const double detune = 1.003;
  std::vector<float> l(frames, 0.f), r(frames, 0.f);
  for (size_t f = 0; f < frames; ++f) {
    const double t = static_cast<double>(f) / sr;
    double env = std::min(1.0, t / 0.9);              // soft attack
    env *= std::min(1.0, std::max(0.0, (seconds - t) / 1.2)); // release
    double s = 0.0;
    for (int i = 0; i < 4; ++i)
      s += (std::sin(2.0 * kPi * freqs[i] * t) +
            std::sin(2.0 * kPi * freqs[i] * detune * t)) * 0.5;
    s *= env * 0.06;
    l[f] = static_cast<float>(s * 0.75);
    r[f] = static_cast<float>(s);
  }
  std::vector<float> inter;
  inter.reserve(frames * 2);
  for (size_t f = 0; f < frames; ++f) {
    inter.push_back(l[f]);
    inter.push_back(r[f]);
  }
  return inter;
}
} // namespace

misuc::project::Session buildDemoSession(const QString& padWavAbsolute) {
  using namespace misuc::project;
  Session s;
  s.setName("Misuc Demo");
  s.setTempo(kTempo);
  s.setTimeSignature(4, 4);
  s.setSampleRate(static_cast<uint32_t>(kSrate));
  s.setLoop(false, 0.0, 0.0);

  const std::string drumsId = s.addTrack("1 - Drums");
  const std::string keysId = s.addTrack("2 - Keys");
  const std::string bassId = s.addTrack("3 - Bass");
  const std::string padId = s.addTrack("4 - Pad");

  Track* drums = s.findTrack(drumsId);
  Track* keys = s.findTrack(keysId);
  Track* bass = s.findTrack(bassId);
  Track* pad = s.findTrack(padId);

  for (int bar = 0; bar < 8; ++bar) {
    drumGroove(*drums, bar);
    chordBass(*keys, *bass, bar);
  }

  AudioClip clip;
  clip.name = "Demo Pad (generated)";
  clip.sourceFile = padWavAbsolute.toStdString();
  clip.startBeats = 0.0;
  clip.durationBeats = 16.0;      // bars 1–4
  clip.fadeInBeats = 2.0;
  clip.fadeOutBeats = 4.0;
  clip.sampleRate = static_cast<uint32_t>(kSrate);
  clip.channels = 2;
  s.addClip(padId, std::move(clip));

  (void)drums;
  (void)keys;
  (void)bass;
  (void)pad;
  return s;
}

bool ensureDemoProject(DemoPaths& out, QString& err) {
  const QString root = QStandardPaths::writableLocation(
                           QStandardPaths::AppDataLocation) +
                       QStringLiteral("/MisucDemo");
  QDir dir(root);
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    err = QStringLiteral("could not create demo dir: %1").arg(root);
    return false;
  }
  out.dir = root;
  out.wav = root + QStringLiteral("/demo_pad.wav");
  out.project = root + QStringLiteral("/demo.misuc");

  // Regenerate the pad asset every call (idempotent, cheap) so the demo never
  // points at a missing file.
  const std::vector<float> samples = makePadWav(8.0);
  std::string werr;
  if (!misuc::audio::writeWavSamples(out.wav.toStdString(), samples.data(),
                                     samples.size() / 2, 2, kSrate, true,
                                     werr)) {
    err = QString::fromStdString(werr);
    return false;
  }

  misuc::project::Session s = buildDemoSession(out.wav);
  const std::string text = s.toJson();
  QFile f(out.project);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    err = QStringLiteral("could not write demo project: %1").arg(out.project);
    return false;
  }
  f.write(text.data(), static_cast<qint64>(text.size()));
  f.close();
  return true;
}

} // namespace misuc::ui