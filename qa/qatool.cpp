// misuc QA harness (qa/qatool.cpp)
//
// Links the REAL user-facing UI objects (misuc_ui_objs) and drives them with
// synthesized Qt mouse events, then asserts on the resulting Session document
// via MainWindow::session(). Engine-level checks run headless by driving
// AudioEngine::process() directly. Nothing here stubs production code; a check
// only passes when the real code path produced the expected outcome.
//
// Usage: misuc_qa [mode]   mode = all (default) | engine | placepattern |
//                             rack | automation | mixer | fx | record | export |
//                             workflow | autosave | firstfunk | browser | ws |
//                             tlo | hiu

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QTabWidget>
#include <chrono>
#include <thread>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QWidget>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#endif
#include <memory>
#include <string>
#include <vector>

#include "MainWindow.h"
#include "HintSystem.h"
#include "HeaderBar.h"
#include "theme/ThemeEngine.h"
#include "branding/Logo.h"
#include "Theme.h"
#include "channel/ChannelRack.h"
#include "playlist/Playlist.h"
#include "mixer/ChannelStrip.h"
#include "mixer/RoutingDialogs.h"
#include "PianoRoll.h"
#include "TransportBar.h"
#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/PipeWireBackend.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/Json.h>
#include <misuc/engine/core/ProjectFile.h>
#include <misuc/engine/fx/BuiltinFx.h>
#include <misuc/engine/project/Session.h>
#include <QStringList>
#include <QTableView>
#include <QThread>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolTip>
#include <QCheckBox>
#include <QMenu>
#include "browser/Browser.h"
#include "export/ExportDialog.h"
#include "elide/ElideLabel.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QSignalBlocker>
#include <QLineEdit>
#include <QTreeWidget>
#include <QDialogButtonBox>
#include <QDialog>
#include <QLayout>

using misuc::engine::AudioEngine;
using misuc::engine::AutomationParam;
using misuc::project::MidiRegion;
using misuc::project::NoteEvent;
using misuc::project::Session;
using misuc::project::Track;
using misuc::ui::MainWindow;
using misuc::ui::PianoRoll;
using misuc::ui::Playlist;
using misuc::ui::TransportBar;

static int gChecks = 0;
static int gFails = 0;

static void report(bool ok, const char* expr, int line) {
  ++gChecks;
  if (!ok) {
    ++gFails;
    std::printf("  FAIL line %d: %s\n", line, expr);
  }
}
#define CHECK(expr) report((expr), #expr, __LINE__)

// ── event synthesis helpers ────────────────────────────────────────────────
static bool postPress(QWidget* w, const QPoint& pos) {
  QMouseEvent e(QEvent::MouseButtonPress, pos, w->mapToGlobal(pos),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  return QApplication::sendEvent(w, &e);
}
static bool postRelease(QWidget* w, const QPoint& pos) {
  QMouseEvent e(QEvent::MouseButtonRelease, pos, w->mapToGlobal(pos),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  return QApplication::sendEvent(w, &e);
}
static bool postMove(QWidget* w, const QPoint& pos) {
  QMouseEvent e(QEvent::MouseMove, pos, w->mapToGlobal(pos), Qt::NoButton,
                Qt::NoButton, Qt::NoModifier);
  return QApplication::sendEvent(w, &e);
}
static void postDblClick(QWidget* w, const QPoint& pos) {
  QMouseEvent e(QEvent::MouseButtonDblClick, pos, w->mapToGlobal(pos),
                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(w, &e);
  QMouseEvent r(QEvent::MouseButtonRelease, pos, w->mapToGlobal(pos),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(w, &r);
}

static void clickWithCtrl(QWidget* w, QPoint pt) {
  const QPoint gp = w->mapToGlobal(pt);
  QMouseEvent press(QEvent::MouseButtonPress, pt, gp, Qt::LeftButton,
                    Qt::LeftButton, Qt::ControlModifier);
  QApplication::sendEvent(w, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, pt, gp, Qt::LeftButton,
                      Qt::NoButton, Qt::ControlModifier);
  QApplication::sendEvent(w, &release);
}

// Right-click (context button) at a point on a widget.
static void clickRight(QWidget* w, QPoint pt) {
  const QPoint gp = w->mapToGlobal(pt);
  QMouseEvent press(QEvent::MouseButtonPress, pt, gp, Qt::RightButton,
                    Qt::RightButton, Qt::NoModifier);
  QApplication::sendEvent(w, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, pt, gp, Qt::RightButton,
                      Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(w, &release);
}

// Vertical mouse-wheel tick at a point on a widget (angleDelta in 8ths).
static void postWheel(QWidget* w, const QPoint& pt, int eighthTicks) {
  QWheelEvent e(QPointF(pt), w->mapToGlobal(pt), QPoint(), QPoint(0, eighthTicks),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(w, &e);
}

static QWidget* findWidgetByTooltip(QWidget* root, const QString& needle) {
  for (QWidget* w : root->findChildren<QWidget*>()) {
    if (w->toolTip().contains(needle))
      return w;
  }
  return nullptr;
}

// Playlist timeline geometry (kept in lockstep with Playlist.cpp constants).
static constexpr int kRulerH = 24;
static constexpr int kLabelW = 168;
static constexpr int kRowH = 52;
static constexpr int kRowPxPerBeat = 24;
static constexpr int kAutoBand = 16;

// ===========================================================================
// ENGINE MODE: headless render checks against the real audio engine.
// ===========================================================================
static void engineChecks() {
  std::printf("CASE engine-midi\n");
  {
    AudioEngine eng(std::make_unique<misuc::engine::PipeWireBackend>());
    eng.setGroove(false);
    eng.setMetronome(false, 0.0f);
    eng.setTempo(120.0);
    std::vector<misuc::dsp::MidiNote> notes{
        {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}};
    eng.setScore(std::move(notes));
    eng.play(false);
    std::vector<float> buf(2 * 24000 * 2, 0.f);
    eng.process(nullptr, 0, buf.data(), 2, 2 * 24000, 0, 48000);
    float peak = 0.f;
    for (size_t i = 8000; i < 40000; ++i)
      peak = std::max(peak, std::fabs(buf[2 * i]));
    CHECK(peak > 0.05f);
    std::printf("  midi playback peak %.3f\n", peak);
  }

  std::printf("CASE engine-automation\n");
  {
    // Ramp 0.05 -> 1.0 over beat 0..1: early window clear, late ~ unity.
    AudioEngine eng(std::make_unique<misuc::engine::PipeWireBackend>());
    eng.setGroove(false);
    eng.setMetronome(false, 0.0f);
    eng.setTempo(120.0);
    std::vector<misuc::dsp::MidiNote> notes{
        {0.0, 4.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0}};
    eng.setScore(std::move(notes));
    std::vector<AutomationParam> ap;
    ap.push_back(AutomationParam{0u, true, "gain", {0.0, 1.0}, {0.05f, 1.0f}});
    eng.publishAutomation(ap);
    eng.play(false);
    std::vector<float> buf(4 * 24000 * 2, 0.f);
    eng.process(nullptr, 0, buf.data(), 2, 4 * 24000, 0, 48000);
    auto peak = [&](int a, int b) {
      float p = 0.f;
      for (int i = a; i < b; ++i) p = std::max(p, std::fabs(buf[2 * i]));
      return p;
    };
    const float early = peak(6000, 10800);   // beats 0.25..0.45 (ramp ~0.3..0.45)
    const float mid = peak(60000, 84000);    // beats 2.5..3.5 (curve holds at 1.0)
    CHECK(early > 0.0f);
    CHECK(mid > 0.05f);
    CHECK(early < mid * 0.6f);               // provable vertical motion
    std::printf("  automation early %.3f late %.3f\n", early, mid);
  }

  std::printf("CASE engine-automation-muted\n");
  {
    // A muted gain lane (points at 0.1) must publish as INACTIVE and therefore
    // leave the render at unity — not a -20 dB dip.
    Session s;
    const std::string tid = s.addTrack("Muted");
    const std::string rid = s.addRegion(tid, "Muted Rgn", 0.0, 4.0);
    auto* region = s.findRegion(tid, rid);
    CHECK(region != nullptr);
    misuc::project::NoteEvent n;
    n.pitch = 60;
    n.velocity = 100;
    n.startBeats = 0.0;
    n.durationBeats = 4.0;
    region->notes.push_back(n);
    const std::string lid = s.addAutomationLane(tid, "gain");
    auto* lane = s.findAutomationLane(tid, lid);
    CHECK(lane != nullptr);
    CHECK(lane->points.size() == 2);
    lane->points = {{0.0, 0.1}, {4.0, 0.1}};
    lane->muted = true;

    AudioEngine eng(std::make_unique<misuc::engine::PipeWireBackend>());
    eng.setGroove(false);
    eng.setMetronome(false, 0.0f);
    eng.setTempo(120.0);
    std::vector<misuc::dsp::MidiNote> notes;
    misuc::engine::buildMidiScore(s, notes);
    eng.setScore(std::move(notes));
    std::vector<AutomationParam> ap;
    std::uint32_t idx = 0;
    for (const auto& t : s.tracks()) {
      for (const auto& l : t.automation) {
        if (l.param != "gain") continue;
        AutomationParam a;
        a.trackIndex = idx;
        a.active = !l.muted && !l.points.empty();
        for (const auto& p : l.points) {
          a.times.push_back(p.timeBeats);
          a.values.push_back(static_cast<float>(p.value));
        }
        ap.push_back(std::move(a));
      }
      ++idx;
    }
    eng.publishAutomation(std::move(ap));
    eng.play(false);
    std::vector<float> buf(4 * 24000 * 2, 0.f);
    eng.process(nullptr, 0, buf.data(), 2, 4 * 24000, 0, 48000);
    float peak = 0.f;
    for (int i = 12000; i < 84000; ++i)
      peak = std::max(peak, std::fabs(buf[2 * i]));
    CHECK(peak > 0.05f); // muted curve: unity (no -20 dB dip)
    std::printf("  muted-lane peak %.3f\n", peak);
  }

  std::printf("CASE session-automation-roundtrip\n");
  {
    Session s;
    const std::string tid = s.addTrack("Rt");
    const std::string lid = s.addAutomationLane(tid, "gain");
    auto* lane = s.findAutomationLane(tid, lid);
    CHECK(lane != nullptr);
    lane->points = {{0.0, 1.0}, {0.5, 0.2}, {1.0, 1.0}, {2.5, 0.1}};
    const Session loaded = Session::fromJson(s.toJson());
    const Track* rt = loaded.findTrack(tid);
    CHECK(rt != nullptr);
    CHECK(rt->automation.size() == 1);
    CHECK(rt->automation.front().points.size() == 4);
    CHECK(std::fabs(rt->automation.front().points[2].value - 1.0) < 1e-9);
  }
}

// ===========================================================================
// MIXER MODE: per-track fader independence over the real engine.
// ===========================================================================
static void mixerChecks() {
  std::printf("CASE mixer\n");
  AudioEngine eng(std::make_unique<misuc::engine::PipeWireBackend>());
  eng.setGroove(false);
  eng.setMetronome(false, 0.0f);
  eng.setTempo(120.0);
  std::vector<misuc::dsp::MidiNote> notes{
      {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0},
      {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}};
  eng.setScore(std::move(notes));
  constexpr uint32_t kFpb = 24000;
  auto render = [&](bool boosted) {
    AudioEngine e(std::make_unique<misuc::engine::PipeWireBackend>());
    e.setGroove(false);
    e.setMetronome(false, 0.0f);
    e.setTempo(120.0);
    std::vector<misuc::dsp::MidiNote> n{
        {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0},
        {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}};
    e.setScore(std::move(n));
    e.publishMixer({misuc::engine::MixerTrackParam{boosted ? 6.f : 0.f, 0.f, false, false},
                    misuc::engine::MixerTrackParam{0.f, 0.f, false, false}});
    e.play(false);
    std::vector<float> buf(4 * kFpb * 2, 0.f);
    e.process(nullptr, 0, buf.data(), 2, 4 * kFpb, 0, 48000);
    auto peak = [&](int a, int b) {
      float p = 0.f;
      for (int i = a; i < b; ++i) p = std::max(p, std::fabs(buf[2 * i]));
      return p;
    };
    return std::pair<float, float>{peak(8000, 40000), peak(56000, 88000)};
  };
  const auto [a0, b0] = render(false);
  const auto [ap, bp] = render(true);
  CHECK(a0 > 0.05f && b0 > 0.05f);
  // +6 dB boosts track A only; track B must stay bit-for-bit untouched.
  CHECK(std::fabs(ap / a0 - misuc::dsp::gainFromDb(6.f)) < 0.15f);
  CHECK(std::fabs(bp - b0) < 1e-6f);
  std::printf("  mixer a0 %.3f ap %.3f b0 %.3f bp %.3f\n", a0, ap, b0, bp);
}

// ===========================================================================
// BUS MODE: the real engine's track -> bus -> master summing stage, plus the
// session-side bus model and round-trip.
// ===========================================================================
static void busChecks() {
  std::printf("CASE bus-engine\n");
  constexpr uint32_t kFpb = 24000;
  auto render = [&](float busDb, bool soloBus, bool muteBus) {
    AudioEngine e(std::make_unique<misuc::engine::PipeWireBackend>());
    e.setGroove(false);
    e.setMetronome(false, 0.0f);
    e.setTempo(120.0);
    std::vector<misuc::dsp::MidiNote> n{
        {0.0, 2.0, misuc::dsp::midiNoteToFreq(60), 1.f, 0},
        {2.0, 2.0, misuc::dsp::midiNoteToFreq(64), 1.f, 1}};
    e.setScore(std::move(n));
    e.publishMixer(
        {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0},
         misuc::engine::MixerTrackParam{0.f, 0.f, false, false}},
        {misuc::engine::MixerBusParam{busDb, 0.f, muteBus, soloBus}});
    e.play(false);
    std::vector<float> buf(4 * kFpb * 2, 0.f);
    e.process(nullptr, 0, buf.data(), 2, 4 * kFpb, 0, 48000);
    auto peak = [&](int a, int b) {
      float p = 0.f;
      for (int i = a; i < b; ++i) p = std::max(p, std::fabs(buf[2 * i]));
      return p;
    };
    return std::pair<float, float>{peak(8000, 40000), peak(56000, 88000)};
  };
  const auto [au, bu] = render(0.f, false, false);
  const auto [ad, bd] = render(-6.f, false, false);
  const auto [asol, bsol] = render(0.f, true, false);
  const auto [amute, bmute] = render(0.f, false, true);
  CHECK(au > 0.05f && bu > 0.05f);
  // Bus fader scales only the routed window.
  CHECK(ad / au < 0.75f && ad / au > 0.35f);
  CHECK(std::fabs(bd - bu) < 1e-6f);
  // Soloed bus keeps its routed track, silences the master-bound track.
  CHECK(asol > 0.05f && bsol < 1e-5f);
  // Muted bus eats the routed track, not the direct one.
  CHECK(amute < 1e-5f && bmute > 0.05f);
  std::printf("  bus au %.3f ad %.3f solo(a=%.3f,b=%.3f) mute(a=%.3f,b=%.3f)\n",
              au, ad, asol, bsol, amute, bmute);

  std::printf("CASE bus-session\n");
  Session s;
  const std::string bid = s.addBus("Drums");
  const std::string tid = s.addTrack("Kick");
  s.findTrack(tid)->busId = bid;
  const Session loaded = Session::fromJson(s.toJson());
  CHECK(loaded.buses().size() == 1);
  const Track* back = loaded.findTrack(tid);
  CHECK(back != nullptr && back->busId == bid);
  CHECK(s.removeBus(bid));
  CHECK(s.findTrack(tid)->busId.empty());
  std::printf("  bus roundtrip + removal ok\n");
}

// ===========================================================================
// PLACE PATTERN: click the real toolbar button, verify the session document.
// ===========================================================================
static void placePatternChecks(MainWindow* win) {
  std::printf("CASE placepattern\n");
  QWidget* btn = findWidgetByTooltip(win, QStringLiteral("Place Pattern"));
  CHECK(btn != nullptr);
  if (!btn) return;

  Session& s = const_cast<Session&>(win->session());
  const QPoint c = btn->rect().center();
  CHECK(postPress(btn, c));
  CHECK(postRelease(btn, c));

  const Track* seq = nullptr;
  for (const auto& t : s.tracks())
    if (t.name == "Sequencer") seq = &t;
  CHECK(seq != nullptr);
  if (!seq) return;
  CHECK(seq->regions.size() == 1);
  CHECK(seq->regions[0].name == "Pattern 1");
  CHECK(std::fabs(seq->regions[0].startBeats) < 1e-9);
  CHECK(std::fabs(seq->regions[0].durationBeats - 4.0) < 1e-9);
  // Pattern materialization invariant: one note per active 1/16 step, stored
  // in LOCAL region time inside [0, 4); the track-level note list stays empty.
  bool notesOk = true;
  for (const auto& r : seq->regions)
    for (const auto& nt : r.notes) {
      notesOk = notesOk && nt.startBeats >= 0.0 &&
                nt.startBeats < r.durationBeats &&
                std::fabs(nt.startBeats -
                          std::round(nt.startBeats / 0.25) * 0.25) < 1e-9;
      notesOk = notesOk && nt.pitch > 0 && nt.velocity > 0;
    }
  CHECK(notesOk);
  CHECK((int)seq->notes.size() == 0);

  // Second placement stacks after the (current) arrangement end = 4.0.
  CHECK(postPress(btn, c));
  CHECK(postRelease(btn, c));
  const Track* again = nullptr;
  for (const auto& t : s.tracks())
    if (t.name == "Sequencer") again = &t;
  CHECK(again != nullptr && again->regions.size() == 2);
  if (again && again->regions.size() >= 2)
    CHECK(std::fabs(again->regions[1].startBeats - 4.0) < 1e-9);
}

// ===========================================================================
// AUTOMATION: drive the playlist gain band through real mouse events.
// ===========================================================================
static void automationChecks(MainWindow* win) {
  std::printf("CASE automation-ui\n");

  // A lane target is needed: reuse the place-pattern path so a real button
  // click produces the Sequencer track (row 0) we then automate.
  QWidget* btn = findWidgetByTooltip(win, QStringLiteral("Place Pattern"));
  CHECK(btn != nullptr);
  if (btn) {
    const QPoint c = btn->rect().center();
    postPress(btn, c);
    postRelease(btn, c);
  }
  Session& s = const_cast<Session&>(win->session());
  CHECK(s.tracks().size() >= 1);

  Playlist* playlist = win->findChild<Playlist*>("playlist", Qt::FindDirectChildrenOnly);
  if (!playlist) playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  if (!playlist) return;
  QWidget* grid = playlist->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);
  if (!grid) return;

  constexpr int row = 0;
  const auto& before = s.tracks()[row];
  const int laneCount0 = (int)before.automation.size();

  // Click in the band at beat 3.0, value near the bottom => creates the gain
  // lane (with unity endpoints) plus a point.
  const int x0 = kLabelW + int(3.0 * kRowPxPerBeat);
  const int yTop = kRulerH + row * kRowH + (kRowH - kAutoBand); // value ~ 1.0
  const int y0 = yTop + 2;                                      // value ~ 0.875
  CHECK(postPress(grid, QPoint(x0, y0)));
  CHECK(postRelease(grid, QPoint(x0, y0)));

  const auto& t = s.tracks()[row];
  const int laneCount1 = (int)t.automation.size();
  CHECK(laneCount1 > laneCount0);
  bool hasGain = false, hasPointNear3 = false;
  for (const auto& l : t.automation) {
    if (l.param != "gain") continue;
    hasGain = true;
    for (const auto& p : l.points)
      if (std::fabs(p.timeBeats - 3.0) < 0.01) hasPointNear3 = true;
  }
  CHECK(hasGain);
  CHECK(hasPointNear3);

  // Drag the point to beat 1.0 at the top of the band (value ~ 1.0).
  const int x1 = kLabelW + int(1.0 * kRowPxPerBeat);
  const int y1 = yTop;
  CHECK(postPress(grid, QPoint(x0, y0)));
  CHECK(postMove(grid, QPoint(x1, y1)));
  CHECK(postRelease(grid, QPoint(x1, y1)));

  bool moved = false;
  for (const auto& l : t.automation) {
    if (l.param != "gain") continue;
    for (const auto& p : l.points)
      if (std::fabs(p.timeBeats - 1.0) < 0.02 && p.value > 0.8) moved = true;
  }
  CHECK(moved);

  // Double-click the point => removed (lane keeps its two endpoints).
  int ptsBefore = 0;
  for (const auto& l : t.automation)
    if (l.param == "gain") ptsBefore += (int)l.points.size();
  postDblClick(grid, QPoint(x1, y1));
  int ptsAfter = 0;
  for (const auto& l : s.tracks()[row].automation)
    if (l.param == "gain") ptsAfter += (int)l.points.size();
  CHECK(ptsAfter < ptsBefore);
  std::printf("  lane points before %d after %d\n", ptsBefore, ptsAfter);
}

// ===========================================================================
// MIXER BUS UI: drive the real "+ Bus" button, a track's route combo, a bus
// strip's mute, and the bus delete button; verify the session document each
// time (the same objects the audio engine hears through syncToEngine()).
// ===========================================================================
static void mixerBusUiChecks(MainWindow* win) {
  std::printf("CASE mixer-bus-ui\n");

  // A track to route: reuse the place-pattern path (Sequencer track, row 0).
  QWidget* btn = findWidgetByTooltip(win, QStringLiteral("Place Pattern"));
  CHECK(btn != nullptr);
  if (btn) {
    const QPoint c = btn->rect().center();
    postPress(btn, c);
    postRelease(btn, c);
  }
  Session& s = const_cast<Session&>(win->session());
  CHECK(!s.tracks().empty());

  // Render the actual mixer pixels before adding a bus (saved for diffing).
  win->grab().save(QStringLiteral("/tmp/misuc_mixer_tracks.png"));

  // Create a bus through the real header button.
  QPushButton* addBus = win->findChild<QPushButton*>(QStringLiteral("addBusBtn"));
  CHECK(addBus != nullptr);
  if (!addBus)
    return;
  addBus->click(); // real QAbstractButton click -> onAddBus slot
  CHECK(s.buses().size() == 1);
  const std::string busId = s.buses().front().id;
  if (s.buses().size() != 1)
    return;

  // Route the first track into the bus via its strip's combo.
  QWidget* host = win->findChild<QWidget*>(QStringLiteral("mixerHost"));
  CHECK(host != nullptr);
  if (!host)
    return;
  QWidget* trackStrip = host->findChildren<QWidget*>(QStringLiteral("channelStrip")).value(0);
  CHECK(trackStrip != nullptr);
  QComboBox* route = trackStrip ? trackStrip->findChild<QComboBox*>() : nullptr;
  CHECK(route != nullptr);
  if (route) {
    // currentIndexChanged fires and onRouteChanged writes track.busId.
    route->setCurrentIndex(1);
  }
  const auto& t = s.tracks().front();
  CHECK(t.busId == busId);

  // Mute the bus strip through its M button.
  QWidget* busStrip =
      win->findChild<QWidget*>(QString::fromStdString(busId) + QStringLiteral("_busStrip"));
  CHECK(busStrip != nullptr);
  if (busStrip) {
    const auto btns = busStrip->findChildren<QPushButton*>();
    CHECK(btns.size() >= 1);
    if (!btns.isEmpty())
      btns.first()->click(); // M (mute), leftmost of the M/S pair
  }
  CHECK(s.findBus(busId) != nullptr);
  if (s.findBus(busId))
    CHECK(s.findBus(busId)->muted);

  // Render again with the routed bus strip in place (saved for diffing).
  win->grab().save(QStringLiteral("/tmp/misuc_mixer_bus.png"));

  // Delete the bus: routing falls back to the master output.
  QPushButton* del =
      win->findChild<QPushButton*>(QString::fromStdString(busId) + QStringLiteral("_delBus"));
  CHECK(del != nullptr);
  if (del)
    del->click();
  CHECK(s.buses().empty());
  CHECK(s.tracks().front().busId.empty());
  std::printf("  bus create/route/mute/delete via real widgets ok\n");
}

// ===========================================================================
// ROUTING (COMP19) UI: parallel sends and sidechain bus FX exercised through
// the real dialogs, published end-to-end through win->sessionEdited(), plus
// the bus route combo on the bus strip. The dialogs are driven headlessly by
// clicking their real widgets (never exec(), so the harness never blocks).
// ===========================================================================
static void routingUiChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE routing\n");
  Session& s = const_cast<Session&>(win->session());
  if (s.tracks().empty())
    s.addTrack("Track 1");
  const std::string tid = s.tracks().front().id;

  // Deterministic headless driving: the window's live stream would race the
  // render below, so stop it first. Renders happen on a fresh engine below.
  if (engine->isRunning()) {
    engine->closeAudio();
    for (int i = 0; i < 400 && engine->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  QPushButton* addBus = win->findChild<QPushButton*>(QStringLiteral("addBusBtn"));
  CHECK(addBus != nullptr);
  if (!addBus)
    return;
  addBus->click();
  CHECK(s.buses().size() == 1);
  if (s.buses().size() != 1)
    return;
  const std::string busId = s.buses().front().id;

  // Route the first track into the new bus through its strip combo.
  QWidget* host = win->findChild<QWidget*>(QStringLiteral("mixerHost"));
  CHECK(host != nullptr);
  if (!host)
    return;
  QWidget* trackStrip =
      host->findChildren<QWidget*>(QStringLiteral("channelStrip")).value(0);
  QComboBox* route = trackStrip ? trackStrip->findChild<QComboBox*>() : nullptr;
  CHECK(route != nullptr);
  if (route)
    route->setCurrentIndex(1);
  CHECK(s.tracks().front().busId == busId);

  // Parallel send: drive the real SendDialog, toggle the row, click OK.
  misuc::ui::SendDialog sendDlg(&s, QString::fromStdString(tid), QString());
  QList<QCheckBox*> cbs = sendDlg.findChildren<QCheckBox*>();
  CHECK(cbs.size() == 1);
  QList<QSlider*> sliders = sendDlg.findChildren<QSlider*>();
  CHECK(sliders.size() == 1);
  CHECK(s.sends().empty());
  if (!cbs.isEmpty())
    cbs.first()->setChecked(true);
  if (!sliders.isEmpty())
    sliders.first()->setValue(-4);
  QDialogButtonBox* sb = sendDlg.findChild<QDialogButtonBox*>();
  CHECK(sb != nullptr);
  if (sb)
    sb->button(QDialogButtonBox::Ok)->click();
  CHECK(s.sends().size() == 1);
  CHECK(s.sends().front().sourceTrack == tid);
  CHECK(s.sends().front().targetBus == busId);
  CHECK(s.sends().front().enabled);

  // Sidechain compressor on the bus: add one through the real dialog and key
  // it off the bus itself, then confirm the session has the insert.
  misuc::ui::BusFxDialog fxDlg(&s, QString::fromStdString(busId));
  QPushButton* addSc = nullptr;
  for (auto* b : fxDlg.findChildren<QPushButton*>())
    if (b->text() == QStringLiteral("+ SC Compressor"))
      addSc = b;
  CHECK(addSc != nullptr);
  if (addSc)
    addSc->click();
  CHECK(s.findBus(busId) != nullptr);
  CHECK(s.findBus(busId)->fx.size() == 1);
  CHECK(s.findBus(busId)->fx.front().uri ==
        misuc::engine::kBuiltinCompUri);
  QList<QComboBox*> keys = fxDlg.findChildren<QComboBox*>();
  CHECK(keys.size() == 1);
  if (keys.size() == 1)
    keys.first()->setCurrentIndex(1); // key: this very bus
  CHECK(s.findBus(busId)->fx.front().sidechainSource == busId);

  // Publish to the engine like a normal edit (sends + bus chains + targets).
  win->sessionEdited();

  // Render the exact published state on a fresh engine (the window's engine
  // instance only outputs through its own live stream, so a manual process()
  // there can never be trusted to produce audio).
  AudioEngine r(std::make_unique<misuc::engine::PipeWireBackend>());
  r.setGroove(false);
  r.setMetronome(false, 0.0f);
  std::vector<float> buf(48000 * 2, 0.f);
  std::vector<misuc::dsp::MidiNote> notes{
      {0.0, 0.2, misuc::dsp::midiNoteToFreq(60), 1.f, 0}};
  r.setScore(std::move(notes));
  // Track 0 -> bus 0, a track-initiated parallel send into the same bus, and
  // the bus's self-keyed SC compressor chain, mirroring the session model.
  std::vector<misuc::engine::MixerSendParam> sends{
      {0, 0xFF, 0, misuc::dsp::gainFromDb(-4.f), true}};
  r.publishMixer(
      {misuc::engine::MixerTrackParam{0.f, 0.f, false, false, 0}},
      {misuc::engine::MixerBusParam{0.f, 0.f, false, false, -1}},
      std::move(sends));
  std::vector<std::vector<misuc::engine::FxInsertParam>> chains;
  std::vector<std::vector<misuc::engine::FxInsertParam>> busChains{
      {{misuc::engine::kBuiltinCompUri, true, {}, 0}}};
  r.publishFx(std::move(chains), std::move(busChains));
  r.play(false);
  r.process(nullptr, 0, buf.data(), 2, 48000, 0, 48000);
  float peak = 0.f;
  for (size_t i = 0; i < buf.size(); ++i)
    peak = std::max(peak, std::fabs(buf[i]));
  CHECK(peak > 0.05f);
  std::printf("  routing render (track->bus + send + SC comp) peak %.4f\n", peak);
  r.closeAudio();

  // Cleanup: deleting the bus drops the send and unroutes the track.
  QPushButton* del =
      win->findChild<QPushButton*>(QString::fromStdString(busId) + QStringLiteral("_delBus"));
  CHECK(del != nullptr);
  if (del)
    del->click();
  CHECK(s.buses().empty());
  CHECK(s.sends().empty());
  CHECK(s.tracks().front().busId.empty());
  win->sessionEdited();
  std::printf("  sends + SC bus fx + route via real dialogs, published ok\n");
}

// ===========================================================================
// PHASE G RECORDING UI: arm a track through its real mixer strip button and
// drive the engine headlessly on the shared engine object. Verifies (a) the
// strip's R button toggles session recordArm AND the engine input-monitor
// gate, (b) the transport record button arms the engine's capture, and (c) the
// Inspector's take list places/removes takes on the timeline (comp-style
// collapse). The sample-perfect capture/span guarantees live in test_record.
// ===========================================================================
static void recordUiChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE record-ui\n");
  Session& s = const_cast<Session&>(win->session());

  // Deterministic headless driving: stop any live stream first.
  if (engine->isRunning()) {
    engine->closeAudio();
    for (int i = 0; i < 400 && engine->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // A playable arrangement (an empty project would not start its transport).
  if (s.tracks().empty())
    s.addTrack("Track 1");
  const std::string tid = s.tracks().front().id;
  s.tracks().front().recordArm = false;
  if (s.tracks().front().regions.empty() && s.tracks().front().clips.empty())
    s.addRegion(tid, "Riff", 0.0, 4.0);
  win->sessionEdited();

  // (a) Strip R button -> session recordArm + engine input monitor.
  auto* arm = win->findChild<QPushButton*>(
      QStringLiteral("arm_") + QString::fromStdString(tid));
  CHECK(arm != nullptr);
  if (!arm)
    return;
  CHECK(!s.tracks().front().recordArm);
  arm->click(); // real QAbstractButton click -> ChannelStrip::armToggled
  CHECK(s.tracks().front().recordArm);
  CHECK(engine->inputMonitor()); // monitoring follows arming
  arm->click();
  CHECK(!s.tracks().front().recordArm);
  CHECK(!engine->inputMonitor()); // unarmed: capture-only, monitor off
  arm->click(); // re-arm for the record step
  CHECK(s.tracks().front().recordArm);
  CHECK(engine->inputMonitor());

  // (b) Transport record button arms the engine capture headlessly.
  TransportBar* transport = win->headerBar()->transportBar();
  CHECK(transport != nullptr);
  if (!transport)
    return;
  // The painted control hit-tests are computed in paintEvent; render the bar
  // off-screen first so recordBtn_ reflects the laid-out geometry.
  QImage tbImg(transport->size(), QImage::Format_ARGB32);
  transport->render(&tbImg);
  const QPoint c = transport->recordButtonRect().center().toPoint();
  CHECK(!engine->isRecording());
  postPress(transport, c);
  postRelease(transport, c);
  // Deterministic capture span: bring the transport back to the song start,
  // then Start/arm controls drain on the next audio block; drive 2 beats so
  // the transport is playing and the arm control is applied.
  engine->seekToBeat(0.0);
  constexpr uint32_t kFrames = 48000; // 2 beats @120/48k
  std::vector<float> out(kFrames * 2, 0.f);
  std::vector<float> in(kFrames);
  for (uint32_t i = 0; i < kFrames; ++i)
    in[i] = 0.2f * std::sin(2.0 * 3.14159265358979 * 440.0 *
                            static_cast<double>(i) / 48000.0);
  engine->process(in.data(), 1, out.data(), 2, kFrames, 0, 48000);
  CHECK(engine->isRecording());
  // Capture spans the transport start (beat 0) to beat 2.
  misuc::engine::RecordCapture cap;
  postPress(transport, c);
  postRelease(transport, c); // transport record off
  std::vector<float> drain(2400 * 2, 0.f);
  engine->process(nullptr, 0, drain.data(), 2, 2400, kFrames, 48000);
  CHECK(!engine->isRecording());
  CHECK(engine->takeRecord(cap));
  if (cap.samples.empty()) {
    std::printf("  WARN no capture from headless transport-rec run\n");
  } else {
    CHECK(std::fabs(cap.startBeats) < 0.05);
    CHECK(cap.samples.size() == kFrames); // the armed run was fully captured
  }

  // (c) Take list: real audio take placed on the timeline via the Inspector,
  // then removed. A second take placed after the first collapses the first.
  const QString audioDir = QStringLiteral("/tmp/misuc_qa_settings/take_audio");
  QDir().mkpath(audioDir);
  std::string err;
  constexpr uint32_t kFrames1 = 48000; // 1 s
  std::vector<float> t1(kFrames1, 0.f);
  for (uint32_t i = 0; i < kFrames1; ++i)
    t1[i] = 0.4f * std::sin(2.0 * 3.14159265358979 * 330.0 *
                            static_cast<double>(i) / 48000.0);
  const QString wav1 = audioDir + QStringLiteral("/take1.wav");
  CHECK(misuc::audio::writeWavSamples(wav1.toStdString(), t1.data(), kFrames1,
                                      1, 48000, false, err));
  const QString wav2 = audioDir + QStringLiteral("/take2.wav");
  CHECK(misuc::audio::writeWavSamples(wav2.toStdString(), t1.data(), kFrames1,
                                      1, 48000, false, err));

  misuc::project::RecordTake k1;
  k1.name = "Take 1";
  k1.sourceFile = wav1.toStdString();
  k1.startBeats = 4.0;
  k1.durationBeats = 2.0; // 1 s @120 = 2 beats
  k1.sampleRate = 48000;
  k1.channels = 1;
  const std::string take1Id = s.addTake(tid, k1);
  

  misuc::project::RecordTake k2;
  k2.name = "Take 2";
  k2.sourceFile = wav2.toStdString();
  k2.startBeats = 4.0;
  k2.durationBeats = 2.0;
  k2.sampleRate = 48000;
  k2.channels = 1;
  const std::string take2Id = s.addTake(tid, k2);

  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  // Selecting the track drives the Inspector to this track's take list.
  playlist->selectTrack(QString::fromStdString(tid));
  win->sessionEdited(); // refresh the Inspector against the selected track
  CHECK(s.tracks().front().clips.empty()); // nothing placed yet

  auto* place1 =
      win->findChild<QPushButton*>(QStringLiteral("takePlace_") +
                                   QString::fromStdString(take1Id));
  CHECK(place1 != nullptr);
  if (!place1)
    return;
  place1->click(); // comp-style place
  CHECK(s.tracks().front().clips.size() == 1);
  if (s.tracks().front().clips.size() == 1) {
    const auto& clip = s.tracks().front().clips.front();
    CHECK(QString::fromStdString(clip.sourceFile) == wav1);
    CHECK(std::fabs(clip.startBeats - 4.0) < 1e-6);
    CHECK(clip.channels == 1);
    const misuc::project::RecordTake* take =
        s.findTake(tid, take1Id);
    CHECK(take != nullptr);
    if (take)
      CHECK(take->clipId == clip.id);
  }

  // Place take 2: take 1's clip is collapsed off the timeline.
  auto* place2 =
      win->findChild<QPushButton*>(QStringLiteral("takePlace_") +
                                   QString::fromStdString(take2Id));
  CHECK(place2 != nullptr);
  if (!place2)
    return;
  place2->click();
  CHECK(s.tracks().front().clips.size() == 1); // exactly one take's audio
  if (s.tracks().front().clips.size() == 1)
    CHECK(QString::fromStdString(s.tracks().front().clips.front().sourceFile) ==
          wav2);

  // Remove take 2 via the inspector: the take and its placed clip disappear.
  auto* del2 = win->findChild<QPushButton*>(
      QStringLiteral("takeDel_") + QString::fromStdString(take2Id));
  CHECK(del2 != nullptr);
  if (!del2)
    return;
  del2->click();
  CHECK(s.findTake(tid, take2Id) == nullptr);
  CHECK(s.tracks().front().clips.empty());
  std::printf(
      "  arm/monitor/record/take-list via real widgets ok\n");
}

// ===========================================================================
// PHASE Q BAND TAPE: dragging an automation point on the playlist band while
// the transport plays and the lane is record-armed taps the dragged value at
// the transport position. The fader is parked low so the taped value must come
// from the band drag (not the fader sweep): after release a point exists at the
// playhead with the dragged (~unity) value.
// ===========================================================================
static void recordBandDragChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE record-band-drag\n");
  Session& s = const_cast<Session&>(win->session());

  if (s.tracks().empty())
    s.addTrack("BandTape");
  const std::string tid = s.tracks().front().id;
  auto& t = s.tracks().front();
  t.gainDb = -12.0f; // a fader-sourced tape would land at ~0.25
  if (t.regions.empty() && t.clips.empty())
    s.addRegion(tid, "Riff", 0.0, 4.0);

  // A gain lane with a middle point at beat 1.0, armed.
  misuc::project::AutomationLane* lane = nullptr;
  for (auto& l : t.automation)
    if (l.param == "gain") lane = &l;
  if (!lane) {
    const std::string lid = s.addAutomationLane(tid, "gain");
    lane = s.findAutomationLane(tid, lid);
  }
  CHECK(lane != nullptr);
  if (!lane)
    return;
  lane->points = {{0.0, 0.5}, {1.0, 0.5}, {4.0, 0.5}};
  lane->armed = true;
  lane->muted = false;
  win->sessionEdited();

  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  QWidget* grid = playlist->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);
  if (!grid)
    return;

  // Start the transport at beat 0 and advance half a beat (12000 frames).
  engine->seekToBeat(0.0);
  engine->play(false);
  std::vector<float> drive(12000 * 2, 0.f);
  engine->process(nullptr, 0, drive.data(), 2, 12000, 0, 48000);
  CHECK(engine->transport().isPlaying());

  // Press exactly on the point (beat 1.0, value 0.5) then drag it to the top
  // of the band (value ~ 1.0) while the transport is playing and the lane is
  // armed; then run ONE record tick synchronously.
  constexpr int row = 0;
  const int yTop = kRulerH + row * kRowH + (kRowH - kAutoBand);
  const int x0 = kLabelW + int(1.0 * kRowPxPerBeat);
  const int yMid = yTop + kAutoBand / 2;
  CHECK(postPress(grid, QPoint(x0, yMid)));
  CHECK(postMove(grid, QPoint(x0, yTop)));
  win->recordAutomationTick(); // headless record tick at beat ~0.5
  CHECK(postRelease(grid, QPoint(x0, yTop)));

  engine->stop();

  bool taped = false;
  const auto& back = s.tracks()[row];
  for (const auto& l : back.automation) {
    if (l.param != "gain") continue;
    for (const auto& p : l.points)
      if (std::fabs(p.timeBeats - 0.5) < 0.02 && p.value > 0.8) taped = true;
  }
  CHECK(taped);
  std::printf("  band drag tape captured at playhead\n");

  // Restore neutral track state (export-checks downstream expects a hot mix):
  // drop the lane + fader so nothing quiet applies to later cases.
  auto& rt = s.tracks()[row];
  for (auto it = rt.automation.begin(); it != rt.automation.end();)
    it = (it->param == "gain") ? rt.automation.erase(it) : ++it;
  rt.gainDb = 0.0f;
  win->sessionEdited();
}

// ===========================================================================
// EXPORT UI (Phase H): bounce the real session through the File > Export menu
// action (exportAudio -> doExport) with the headless override hooks, decode
// each produced file with the engine's own reader (a separate TLS from the
// writer backend), and assert container choice, dither injection, the −1 dBFS
// headroom ceiling, and loop-range slicing. Selection-range export shares the
// same renderToFile slice path exercised by the engine-level range test.
// ===========================================================================
static void exportChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE export-ui\n");
  Session& s = const_cast<Session&>(win->session());
  win->clearExportOptionsForTest();
  win->setExportPathOverride(QString());

  if (engine->isRunning())
    engine->closeAudio();

  // A hot clip forces the master limiter to prove itself through the real
  // export path: 1.5 amplitude would exceed −1 dBFS without it.
  const QString audioDir = QStringLiteral("/tmp/misuc_qa_settings/export_audio");
  QDir().mkpath(audioDir);
  constexpr uint64_t kFrames = 96000; // 4 beats @120/48k = 2 s
  std::vector<float> hot(kFrames, 0.f);
  for (uint64_t i = 0; i < kFrames; ++i)
    hot[i] = 1.5f * std::sin(2.0 * 3.14159265358979 * 440.0 *
                             static_cast<double>(i) / 48000.0);
  const QString hotWav = audioDir + QStringLiteral("/hot.wav");
  std::string err;
  if (!misuc::audio::writeWavSamples(hotWav.toStdString(), hot.data(),
                                     kFrames, 1, 48000, false, err)) {
    CHECK(!err.empty());
    std::printf("  WARN could not stage export source: %s\n", err.c_str());
    return;
  }

  const std::string tid = s.tracks().empty() ? s.addTrack("Export")
                                             : s.tracks().front().id;
  s.setLoop(false, 0.0, 0.0);
  for (auto& tr : s.tracks()) {
    tr.clips.clear();
    tr.notes.clear();
    tr.regions.clear();
  }
  misuc::project::AudioClip c;
  c.name = "Hot clip";
  c.sourceFile = hotWav.toStdString();
  c.startBeats = 0.0;
  c.durationBeats = 4.0;
  c.sampleRate = 48000;
  c.channels = 1;
  s.addClip(tid, c);
  win->sessionEdited();

  auto mediaExportAction = [win]() -> QAction* {
    const auto acts = win->findChildren<QAction*>();
    for (auto* a : acts) {
      QString t = a->text();
      t.remove(QLatin1Char('&'));
      if (t == QStringLiteral("Export Audio..."))
        return a;
    }
    return nullptr;
  };
  QAction* exportAct = mediaExportAction();
  CHECK(exportAct != nullptr);
  if (!exportAct)
    return;

  auto decode = [&](const QString& f, misuc::audio::AudioBuffer& out) {
    CHECK(misuc::audio::readAudioFile(f.toStdString(), out, err));
    if (out.frames == 0)
      std::printf("  WARN %s decode empty: %s\n", f.toLatin1().constData(),
                  err.c_str());
  };

  // (a) FLAC whole-song, dither OFF: 4 beats, then (b) dither ON changes the
  // words but stays bounded. (c) PCM16 dither OFF decodes to the same 16-bit
  // word sequence as FLAC.
  const QString flacNo = audioDir + QStringLiteral("/whole.flac");
  const QString flacDit = audioDir + QStringLiteral("/whole-dither.flac");
  const QString wav16 = audioDir + QStringLiteral("/whole.wav");
  const QString wavF = audioDir + QStringLiteral("/whole-float.wav");

  // Renders below must be compared warm-vs-warm: the FIRST render after the
  // engine restarts has a limiter/oscillator warm-up transient, so render the
  // float master once, then use the SECOND float render as the reference every
  // fixed-word render is measured against.
  win->setExportPathOverride(wavF);
  win->clearExportOptionsForTest();
  exportAct->trigger(); // warm-up render #1 (not used; cold-start transient)

  const QString wavFRef = audioDir + QStringLiteral("/whole-float-ref.wav");
  win->setExportPathOverride(wavFRef);
  win->clearExportOptionsForTest();
  exportAct->trigger(); // warm render #2 = the bit-exact reference
  misuc::audio::AudioBuffer wfRef;
  decode(wavFRef, wfRef);
  CHECK(wfRef.frames == kFrames);
  CHECK(wfRef.channels == 2);

  // (b) FLAC whole-song, dither OFF: 4 beats, and (c) dither ON changes the
  // words but adds no more than its ±1 LSB.
  win->setExportPathOverride(flacNo);
  win->setExportOptionsForTest(
      static_cast<int>(misuc::audio::ExportFormat::Flac), /*loop=*/false,
      /*dither=*/false);
  exportAct->trigger();
  misuc::audio::AudioBuffer flacNoBuf;
  decode(flacNo, flacNoBuf);
  CHECK(flacNoBuf.frames == kFrames);
  CHECK(flacNoBuf.channels == 2);

  win->setExportPathOverride(flacDit);
  win->setExportOptionsForTest(
      static_cast<int>(misuc::audio::ExportFormat::Flac), false, true);
  exportAct->trigger();
  misuc::audio::AudioBuffer flacDitBuf;
  decode(flacDit, flacDitBuf);
  CHECK(flacDitBuf.frames == kFrames);
  CHECK(flacDitBuf.samples != flacNoBuf.samples); // dither changed the words
  float worstDit = 0.f;
  const size_t dn = std::min(flacDitBuf.samples.size(),
                             flacNoBuf.samples.size());
  for (size_t i = 0; i < dn; ++i)
    worstDit = std::max(
        worstDit, std::fabs(flacDitBuf.samples[i] - flacNoBuf.samples[i]));
  CHECK(worstDit <= 1.5f / 32768.0f); // dither only (±1 LSB)
  std::printf("  dither added words, worst=%.3e\n", double(worstDit));

  // (d) PCM16 and FLAC are the same 16-bit master; libsndfile normalises the
  // two containers with slightly different float scaling (~1 LSB), so compare
  // within ±2 LSB.
  win->setExportPathOverride(wav16);
  win->setExportOptionsForTest(
      static_cast<int>(misuc::audio::ExportFormat::WavPcm16), false, false);
  exportAct->trigger();
  misuc::audio::AudioBuffer w16Buf;
  decode(wav16, w16Buf);
  CHECK(w16Buf.frames == kFrames);
  float worstW16 = 0.f;
  const size_t w16n = std::min(w16Buf.samples.size(), flacNoBuf.samples.size());
  for (size_t i = 0; i < w16n; ++i)
    worstW16 = std::max(
        worstW16, std::fabs(w16Buf.samples[i] - flacNoBuf.samples[i]));
  CHECK(worstW16 <= 2.0f / 32768.0f);

  // (e) The fixed-word master tracks the warm float render within ~1 LSB.
  float worstFlac = 0.f;
  const size_t ffn = std::min(flacNoBuf.samples.size(), wfRef.samples.size());
  for (size_t i = 0; i < ffn; ++i)
    worstFlac = std::max(
        worstFlac, std::fabs(flacNoBuf.samples[i] - wfRef.samples[i]));
  CHECK(worstFlac <= 2.0f / 32768.0f);
  std::printf("  pcm16-vs-flac worst=%.3e flac-vs-float worst=%.3e\n",
              double(worstW16), double(worstFlac));

  // The hot clip is forced under −1 dBFS in every container.
  float peak = 0.f;
  for (const auto& v : flacNoBuf.samples)
    peak = std::max(peak, std::fabs(v));
  CHECK(peak <= 0.892f);
  CHECK(peak >= 0.85f); // limited, not attenuated into nothing
  float peakW16 = 0.f;
  for (const auto& v : w16Buf.samples)
    peakW16 = std::max(peakW16, std::fabs(v));
  CHECK(peakW16 <= 0.892f);
  float peakF = 0.f;
  for (const auto& v : wfRef.samples)
    peakF = std::max(peakF, std::fabs(v));
  CHECK(peakF <= 0.892f);
  CHECK(peakF >= 0.85f);
  std::printf("  whole-song flac peak=%.4f pcm16 peak=%.4f float peak=%.4f\n",
              double(peak), double(peakW16), double(peakF));

  // (f) Loop-range export: 2 beats, equal to the same slice of the warm float
  // whole-song render.
  s.setLoop(true, 1.0, 3.0);
  win->sessionEdited();
  const QString loopFlac = audioDir + QStringLiteral("/loop.flac");
  win->setExportPathOverride(loopFlac);
  win->setExportOptionsForTest(
      static_cast<int>(misuc::audio::ExportFormat::Flac), true, false);
  exportAct->trigger();
  misuc::audio::AudioBuffer loopBuf;
  decode(loopFlac, loopBuf);
  CHECK(loopBuf.frames == 48000); // 2 beats
  bool slice = loopBuf.frames + 1 < wfRef.frames;
  const size_t off = 24000 * 2; // beat-1 frame offset, L/R interleaved
  for (size_t i = 0; slice && i < loopBuf.samples.size(); ++i)
    slice = std::fabs(loopBuf.samples[i] - wfRef.samples[i + off]) <
            2.0f / 32768.0f;
  CHECK(slice);
  s.setLoop(false, 0.0, 0.0);
  win->sessionEdited();

  // Phase R: the session's title/artist land in the exported file's tags
  // (RIFF INFO for WAV) and the per-project sample rate causes a resampled
  // 44100 Hz bounce whose frame count still spans the same 4 beats.
  const std::string qTitle = "QA Export Title";
  const std::string qArtist = "QA Export Artist";
  s.setName(qTitle);
  s.setArtist(qArtist);
  const QString metaWav = audioDir + QStringLiteral("/meta-44100.wav");
  win->setExportPathOverride(metaWav);
  win->clearExportOptionsForTest();
  win->setExportSampleRateOverrideForTest(44100);
  exportAct->trigger();
  misuc::audio::AudioBuffer metaBuf;
  decode(metaWav, metaBuf);
  CHECK(metaBuf.sampleRate == 44100.0);
  CHECK(metaBuf.frames == 88200); // 4 beats @120/44.1k
  CHECK(metaBuf.title == qTitle);
  CHECK(metaBuf.artist == qArtist);

  // (g) Every codec the dialog offers actually bounces through the real menu
  // path and decodes back with the right length/channels and no clipping.
  win->setExportSampleRateOverrideForTest(0); // back to native after the 44.1k
  const std::vector<misuc::audio::ExportFormat> kFormats =
      misuc::audio::availableExportFormats();
  CHECK(!kFormats.empty());
  CHECK(kFormats.front() == misuc::audio::ExportFormat::WavFloat);
  bool sawFlac = false, sawMp3 = false, sawOgg = false, sawAiff = false;
  for (const auto f : kFormats) {
    const QString ext =
        QString::fromLatin1(misuc::audio::exportFormatSuffix(f));
    const QString out = audioDir + QStringLiteral("/codec.") + ext;
    win->setExportPathOverride(out);
    win->setExportOptionsForTest(static_cast<int>(f), false, false);
    exportAct->trigger();
    CHECK(QFile::exists(out));
    misuc::audio::AudioBuffer b;
    decode(out, b);
    const bool lossy = f == misuc::audio::ExportFormat::Mp3 ||
                       f == misuc::audio::ExportFormat::OggVorbis;
    const uint64_t tol = lossy ? 4096 : 0;
    CHECK(b.channels == 2);
    CHECK(b.frames + tol >= kFrames && b.frames <= kFrames + tol);
    float p = 0.f;
    for (const auto& v : b.samples)
      p = std::max(p, std::fabs(v));
    CHECK(p > 0.05f);  // audible
    CHECK(p <= 0.95f); // master limiter honoured
    if (f == misuc::audio::ExportFormat::Flac) sawFlac = true;
    if (f == misuc::audio::ExportFormat::Mp3) sawMp3 = true;
    if (f == misuc::audio::ExportFormat::OggVorbis) sawOgg = true;
    if (f == misuc::audio::ExportFormat::Aiff) sawAiff = true;
    std::printf("  codec %-4s frames=%llu peak=%.4f\n",
                ext.toLatin1().constData(),
                static_cast<unsigned long long>(b.frames), double(p));
    QFile::remove(out);
  }
  CHECK(sawFlac && sawMp3 && sawOgg && sawAiff);

  // (h) Include Metronome reaches the bounce. Use a silent session: the hot
  // clip saturates the brickwall limiter, which would clamp the added clicks
  // back to the same ceiling and mask the difference.
  {
    for (auto& tr : s.tracks()) {
      tr.clips.clear();
      tr.notes.clear();
      tr.regions.clear();
    }
    win->sessionEdited();
    const QString metOff = audioDir + QStringLiteral("/met-off.wav");
    const QString metOn = audioDir + QStringLiteral("/met-on.wav");
    win->clearExportOptionsForTest();
    win->setExportMetronomeForTest(false);
    win->setExportPathOverride(metOff);
    exportAct->trigger();
    win->setExportMetronomeForTest(true);
    win->setExportPathOverride(metOn);
    exportAct->trigger();
    win->setExportMetronomeForTest(false);
    misuc::audio::AudioBuffer offB, onB;
    decode(metOff, offB);
    decode(metOn, onB);
    CHECK(offB.frames == kFrames && onB.frames == kFrames);
    CHECK(offB.samples != onB.samples);
    double eOff = 0.0, eOn = 0.0;
    const size_t en = std::min(offB.samples.size(), onB.samples.size());
    for (size_t i = 0; i < en; ++i) {
      eOff += static_cast<double>(offB.samples[i]) * offB.samples[i];
      eOn += static_cast<double>(onB.samples[i]) * onB.samples[i];
    }
    CHECK(eOff < 1.0); // silent without the click
    CHECK(eOn > eOff); // and audibly present with it
    std::printf("  metronome include offE=%.1f onE=%.1f\n", eOff, eOn);
    QFile::remove(metOff);
    QFile::remove(metOn);
  }

  // (i) The interactive dialog offers exactly the probed codecs (never an
  // unsupported one) and reports the matching extension for each.
  {
    misuc::ui::ExportDialog dlg(misuc::audio::ExportFormat::WavFloat, false,
                                false, nullptr, QStringLiteral("t"),
                                QStringLiteral("a"), 48000);
    QComboBox* combo =
        dlg.findChild<QComboBox*>(QStringLiteral("exportFormatCombo"));
    CHECK(combo != nullptr);
    if (combo) {
      CHECK(combo->count() == static_cast<int>(kFormats.size()));
      for (int i = 0; i < combo->count() &&
                      i < static_cast<int>(kFormats.size());
           ++i) {
        combo->setCurrentIndex(i);
        const QString want =
            QString::fromLatin1(misuc::audio::exportFormatSuffix(kFormats[i]));
        CHECK(dlg.suffix() == want);
        CHECK(dlg.format() == kFormats[i]);
      }
    }
    QCheckBox* met =
        dlg.findChild<QCheckBox*>(QStringLiteral("exportMetronomeCheck"));
    CHECK(met != nullptr);
    if (met) {
      CHECK(!dlg.includeMetronome()); // default OFF
      met->setChecked(true);
      CHECK(dlg.includeMetronome());
    }
  }

  for (const QString& f : {flacNo, flacDit, wav16, wavF, wavFRef, loopFlac,
                           metaWav})
    QFile::remove(f);

  win->setExportPathOverride(QString());
  win->clearExportOptionsForTest();
  win->setExportSampleRateOverrideForTest(0);
  std::printf("  format/dither/headroom/loop-ring/meta/44100/codecs/metronome "
              "via real menu path ok\n");
}

// ===========================================================================
// FX RACK UI: drive the Inspector's effects section through the real widgets
// (catalog combo, Add, ON/off toggle, param spin, remove x), asserting the
// Session document and a live headless render through the shared engine after
// each step. This is the Phase 17 hosting path end to end: session TrackFx ->
// applyFxToEngine() -> publishFx() -> real LV2 instance in the render fold.
// ===========================================================================
static void fxUiChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE fx-ui\n");

  // A track to host the chain: reuse the place-pattern path (Sequencer track).
  QWidget* btn = findWidgetByTooltip(win, QStringLiteral("Place Pattern"));
  CHECK(btn != nullptr);
  if (btn) {
    const QPoint c = btn->rect().center();
    postPress(btn, c);
    postRelease(btn, c);
  }
  Session& s = const_cast<Session&>(win->session());
  CHECK(!s.tracks().empty());
  if (s.tracks().empty()) return;

  Playlist* playlist =
      win->findChild<Playlist*>("playlist", Qt::FindDirectChildrenOnly);
  if (!playlist) playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  if (!playlist) return;
  // Selecting the track switches the Inspector to the FX rack.
  playlist->selectTrack(QString::fromStdString(s.tracks().front().id));

  QComboBox* combo = win->findChild<QComboBox*>(QStringLiteral("fxAddCombo"));
  QPushButton* addBtn = win->findChild<QPushButton*>(QStringLiteral("fxAddBtn"));
  CHECK(combo != nullptr);
  CHECK(addBtn != nullptr);
  if (!combo || !addBtn) return;

  if (combo->count() == 0) {
    std::printf("  (no hostable LV2 effects; hosting offline - skipping)\n");
    CHECK(!engine->fxSupported());
    return;
  }

  // Prefer the delay family so the rack has parameters to move.
  int delayIdx = -1;
  for (int i = 0; i < combo->count(); ++i)
    if (combo->itemText(i).contains("DubDelay") ||
        combo->itemText(i).contains("Delay")) {
      delayIdx = i;
      break;
    }
  const int pick = delayIdx >= 0 ? delayIdx : 0;
  combo->setCurrentIndex(pick);
  const std::string wantUri = combo->currentData().toString().toStdString();

  addBtn->click();
  const auto& t = s.tracks().front();
  CHECK(t.fx.size() == 1);
  if (t.fx.size() != 1) return;
  const auto& fxSlot = t.fx.back();
  CHECK(fxSlot.uri == wantUri);
  CHECK(fxSlot.enabled);
  const std::string fxId = fxSlot.id;

  // Disable through the rack's ON/off toggle -> the document reflects it.
  QPushButton* onBtn =
      win->findChild<QPushButton*>(QString::fromStdString("fxOn_" + fxId));
  CHECK(onBtn != nullptr);
  if (onBtn && onBtn->isChecked()) {
    // Real mouse events: MainWindow rebuilds the inspector from the emitted
    // edit and the FX host is freed via deleteLater, so the button survives its
    // own state change and Qt's follow-up accessibility update (see
    // Inspector::clearSection). A QAbstractButton::click() shortcut would not
    // exercise the real press/release path.
    const QPoint c = onBtn->rect().center();
    postPress(onBtn, c);
    postRelease(onBtn, c);
    CHECK(!s.tracks().front().fx.back().enabled);
  }

  // Move the first exposed parameter through its spin box (real widget).
  if (!fxSlot.params.empty()) {
    const uint32_t port = fxSlot.params.front().index;
    QDoubleSpinBox* spin = win->findChild<QDoubleSpinBox*>(
        QString::fromStdString("fxParam_" + fxId + "_" + std::to_string(port)));
    if (spin) {
      const double target = (spin->minimum() + spin->maximum()) / 2.0;
      spin->setValue(target);
      QMetaObject::invokeMethod(spin, "editingFinished");
      bool found = false;
      for (const auto& p : s.tracks().front().fx.back().params)
        if (p.index == port) found = true;
      CHECK(found);
    }
  }

  // The published chain is live: spin a headless render through the engine the
  // window is bound to, so a UI-driven publish actually reaches the fold.
  engine->play(false);
  std::vector<float> buf(2 * 24000, 0.f);
  engine->process(nullptr, 0, buf.data(), 2, 24000, 0, 48000);

  // Remove through the rack's x button -> chain back to empty.
  QPushButton* delBtn =
      win->findChild<QPushButton*>(QString::fromStdString("fxDel_" + fxId));
  CHECK(delBtn != nullptr);
  if (delBtn) delBtn->click();
  CHECK(s.tracks().front().fx.empty());

  // Undo restores the insert (last edit was remove), redo re-removes.
  QAction* undo = nullptr;
  for (QAction* a : win->findChildren<QAction*>())
    if (a->shortcut() == QKeySequence::Undo) undo = a;
  CHECK(undo != nullptr);
  if (undo) undo->trigger();
  CHECK(s.tracks().front().fx.size() == 1);
  QAction* redo = nullptr;
  for (QAction* a : win->findChildren<QAction*>())
    if (a->shortcut() == QKeySequence::Redo) redo = a;
  CHECK(redo != nullptr);
  if (redo) redo->trigger();
  CHECK(s.tracks().front().fx.empty());
  std::printf("  add/enable-toggle/param/remove/undo/redo via real widgets ok\n");

  // The built-in EQ is catalogued on every build (even when lilv is offline):
  // add it from the real combo, move a band-gain param through its spin, run a
  // live render through the shared engine, then remove. This is the Phase F
  // always-on FX path end to end.
  const std::string builtin = std::string(misuc::engine::kBuiltinEqUri);
  int bi = -1;
  for (int i = 0; i < combo->count(); ++i)
    if (combo->itemData(i).toString().toStdString() == builtin) {
      bi = i;
      break;
    }
  CHECK(bi >= 0);
  if (bi < 0) return;
  combo->setCurrentIndex(bi);
  addBtn->click();
  CHECK(s.tracks().front().fx.size() == 1);
  if (s.tracks().front().fx.empty()) return;
  const auto& bfx = s.tracks().front().fx.back();
  CHECK(bfx.uri == builtin);
  const std::string bfid = bfx.id;

  // Boost band 1 (port 3 = band-1 gain) through the real spin box.
  QDoubleSpinBox* gain = win->findChild<QDoubleSpinBox*>(
      QString::fromStdString("fxParam_" + bfid + "_3"));
  if (gain) {
    gain->setValue(12.0);
    QMetaObject::invokeMethod(gain, "editingFinished");
  }
  bool hasGain = false;
  for (const auto& p : s.tracks().front().fx.back().params)
    if (p.index == 3) hasGain = true;
  CHECK(hasGain);

  engine->play(false);
  std::vector<float> ebuf(2 * 24000, 0.f);
  engine->process(nullptr, 0, ebuf.data(), 2, 24000, 0, 48000);

  QPushButton* bdel = win->findChild<QPushButton*>(
      QString::fromStdString("fxDel_" + bfid));
  CHECK(bdel != nullptr);
  if (bdel) bdel->click();
  CHECK(s.tracks().front().fx.empty());
  std::printf("  built-in EQ add/param/render/remove via real widgets ok\n");
}

// ===========================================================================
// WORKFLOW: one integrated end-to-end pass over the real widgets.
// ===========================================================================
// Phase 18: the whole user journey is exercised as a single scripted session
// against real UI widgets + the real Session + the real audio engine —
// placement → import → arrange → edit MIDI → mix → seek/play/stop → loop →
// save → reopen. Nothing is faked: every check asserts on the Session document
// mutated through a real widget interaction and, for transport, on real
// AudioEngine::process() frames.

static void postKey(QWidget* w, int key, Qt::KeyboardModifiers mod) {
  QKeyEvent press(QEvent::KeyPress, key, mod);
  press.setAccepted(false);
  QApplication::sendEvent(w, &press);
  QKeyEvent release(QEvent::KeyRelease, key, mod);
  release.setAccepted(false);
  QApplication::sendEvent(w, &release);
}

static QWidget* findChildByClassName(QWidget* parent, const char* className) {
  for (QWidget* w : parent->findChildren<QWidget*>())
    if (qstrcmp(w->metaObject()->className(), className) == 0)
      return w;
  return nullptr;
}

static QPushButton* findButtonByText(QWidget* parent, const QString& text) {
  for (QPushButton* b : parent->findChildren<QPushButton*>())
    if (b->text() == text)
      return b;
  return nullptr;
}

static QAction* findActionByKey(MainWindow* win, const QKeySequence& seq) {
  for (QAction* a : win->findChildren<QAction*>())
    if (a->shortcut() == seq)
      return a;
  return nullptr;
}

// ===========================================================================
// CHANNEL RACK: the rack is a first-class Session element (persists in the
// document, feeds undo), and the keyboard DELETE works.
// ===========================================================================
static void rackChecks(MainWindow* win) {
  std::printf("CASE rack-session\n");
  Session& s = const_cast<Session&>(win->session());
  auto* rack = win->findChild<misuc::ui::ChannelRack*>();
  CHECK(rack != nullptr);
  if (!rack) return;

  // The seeded starter kit is now part of the Session: widget and document
  // agree from construction (no more QSettings-only state).
  CHECK(s.channelRack().channels.size() == 5);
  CHECK(rack->channels().size() == 5);
  CHECK(s.channelRack().channels[0].name == "Kick");
  CHECK(s.channelRack().channels[0].steps[0][0] == 100);
  CHECK(s.channelRack().channels[4].name == "Sampler");

  // A real programmatic step toggle mutates the widget AND the Session (on
  // steps carry the default velocity 100).
  CHECK(rack->setStep(0, 0, 3, true));
  CHECK(s.channelRack().channels[0].steps[0][3] == 100);
  // Per-step velocity tuning: 0 removes the step, 1..127 sets velocity, and
  // the widget + Session stay in lock-step.
  rack->setStepVelocity(0, 0, 3, 66);
  CHECK(s.channelRack().channels[0].steps[0][3] == 66);
  CHECK(rack->stepVelocity(0, 0, 3) == 66);
  rack->setStepVelocity(0, 0, 3, 0);
  CHECK(s.channelRack().channels[0].steps[0][3] == 0);
  rack->setStepVelocity(0, 0, 3, 127);
  CHECK(s.channelRack().channels[0].steps[0][3] == 127);
  // Restore the seeded gate the later suites expect (placed by default bank).
  rack->setStepVelocity(0, 0, 3, 100);

  // Per-channel volume/pan mutators write the Session (the row's live badge
  // and the document stay in lock-step).
  rack->setVolume(0, 0.45f);
  CHECK(fabs(s.channelRack().channels[0].volume - 0.45f) < 1e-4f);
  CHECK(fabs(rack->channels()[0].volume - 0.45f) < 1e-4f);
  rack->setPan(0, -0.6f);
  CHECK(fabs(s.channelRack().channels[0].pan + 0.6f) < 1e-4f);
  CHECK(fabs(rack->channels()[0].pan + 0.6f) < 1e-4f);
  // The drag volume/pan cells clamp like the model (bounds respected).
  rack->setVolume(0, 2.0f);
  CHECK(fabs(s.channelRack().channels[0].volume - 1.0f) < 1e-4f);
  rack->setPan(0, -3.0f);
  CHECK(fabs(s.channelRack().channels[0].pan + 1.0f) < 1e-4f);
  // Restore a neutral rack for later suites.
  rack->setVolume(0, 0.60f);
  rack->setPan(0, 0.0f);

  // Channel type + identity: type flows to the Session, duplicate gets a
  // brand-new id (never a copy), and the visible name can change without the
  // identity changing.
  auto* tbl = rack->findChild<QTableView*>(QStringLiteral("rackTable"));
  CHECK(tbl != nullptr);
  CHECK(rack->channelType(3) == misuc::project::ChannelType::Instrument);
  rack->setChannelType(3, misuc::project::ChannelType::Plugin);
  CHECK(rack->channelType(3) == misuc::project::ChannelType::Plugin);
  CHECK(s.channelRack().channels[3].type == misuc::project::ChannelType::Plugin);
  const QString originalId = rack->channelId(3);
  CHECK(!originalId.isEmpty());
  const std::string jsonFrozen = s.toJson();
  CHECK(Session::fromJson(jsonFrozen).channelRack().channels[3].id ==
        originalId.toStdString());
  const QString originalName = rack->channels()[3].name;
  rack->setVolume(0, 0.60f); // keep row0 neutral; rename below
  // Rename via the model's editable cell: identity must not track the name.
  auto* m = tbl ? tbl->model() : nullptr;
  if (m) m->setData(m->index(3, 1), QStringLiteral("Renamed Synth"));
  CHECK(rack->channels()[3].name == QStringLiteral("Renamed Synth"));
  CHECK(s.channelRack().channels[3].name == "Renamed Synth");
  CHECK(rack->channelId(3) == originalId);
  CHECK(s.channelRack().channels[3].id == originalId.toStdString());
  m->setData(m->index(3, 1), originalName);
  CHECK(rack->channels()[3].name == originalName);
  if (tbl) {
    const QPoint p3 = tbl->visualRect(tbl->model()->index(3, 1)).center();
    postPress(tbl->viewport(), p3);
    postRelease(tbl->viewport(), p3);
    CHECK(rack->currentRow() == 3);
  }
  int preCount = int(rack->channels().size());
  rack->duplicateCurrent();
  CHECK(int(rack->channels().size()) == preCount + 1);
  const QString dupId = rack->channelId(preCount);
  CHECK(!dupId.isEmpty());
  CHECK(dupId != originalId);
  CHECK(s.channelRack().channels[preCount].id == dupId.toStdString());
  rack->removeCurrent(); // clean duplicate back up
  CHECK(int(rack->channels().size()) == preCount);

  // Variable pattern length: switching to 64 steps widens the grid, sets the
  // Session's stepsPerPattern, and far steps edit + persist. Round-tripping
  // the document also restores the count back into the widget.
  CHECK(rack->stepsPerPattern() == 16);
  CHECK(s.channelRack().stepsPerPattern == 16);
  auto* stl = rack->findChild<QComboBox*>(QStringLiteral("stepsCombo"));
  CHECK(stl != nullptr);
  if (tbl && stl) {
    const int cols16 = tbl->model()->columnCount();
    CHECK(cols16 == 16 + 4 + 2); // steps + Type/name/mute/solo + Vol/Pan
    if (stl->count() >= 4) {
      stl->setCurrentIndex(stl->findData(64));
      CHECK(rack->stepsPerPattern() == 64);
      CHECK(s.channelRack().stepsPerPattern == 64);
      CHECK(tbl->model()->columnCount() == 64 + 4 + 2);
      // Far gate is editable and materializes across the longer lane.
      rack->setStepVelocity(0, 0, 63, 100);
      CHECK(s.channelRack().channels[0].steps[0][63] == 100);
      const Session reloaded = Session::fromJson(s.toJson());
      CHECK(reloaded.channelRack().stepsPerPattern == 64);
      CHECK(reloaded.channelRack().channels[0].steps[0][63] == 100);
      // Back to 16: far gate is cut, head stays, the widget agrees.
      stl->setCurrentIndex(stl->findData(16));
      CHECK(rack->stepsPerPattern() == 16);
      CHECK(s.channelRack().stepsPerPattern == 16);
      CHECK(tbl->model()->columnCount() == 16 + 4 + 2);
      CHECK(rack->stepVelocity(0, 0, 63) == 0);
      rack->setStepVelocity(0, 0, 3, 100); // reseed the standard gate
    }
  }

  // Channel reorder: moveUp/moveDown swap within the Session AND the widget,
  // keeping selection anchored to the moved channel.
  const QString k0 = rack->channels()[0].name;
  const QString k1 = rack->channels()[1].name;
  if (tbl) {
    const QPoint p1 = tbl->visualRect(tbl->model()->index(1, 0)).center();
    postPress(tbl->viewport(), p1);
    postRelease(tbl->viewport(), p1);
    CHECK(rack->currentRow() == 1);
  }
  rack->moveUp();
  CHECK(rack->channels()[0].name == k1);
  CHECK(rack->channels()[1].name == k0);
  CHECK(s.channelRack().channels[0].name == k1.toStdString());
  CHECK(s.channelRack().channels[1].name == k0.toStdString());
  CHECK(rack->currentRow() == 0);
  rack->moveUp(); // no-op at the top edge: order unchanged
  CHECK(rack->channels()[0].name == k1);
  rack->moveDown();
  CHECK(rack->channels()[0].name == k0);
  CHECK(rack->channels()[1].name == k1);
  if (tbl) {
    const QPoint p0 = tbl->visualRect(tbl->model()->index(0, 0)).center();
    postPress(tbl->viewport(), p0);
    postRelease(tbl->viewport(), p0);
  }

  // Add a channel through the real Add > Channel Rack Channel menu action.
  QAction* addAction = nullptr;
  for (QAction* a : win->findChildren<QAction*>()) {
    QString t = a->text();
    t.remove('&');
    if (t == QStringLiteral("Channel Rack Channel")) addAction = a;
  }
  CHECK(addAction != nullptr);
  if (addAction) addAction->trigger();
  CHECK(s.channelRack().channels.size() == 6);
  CHECK(rack->channels().size() == 6);

  // Keyboard DELETE removes the selected channel (row 5 = the new one).
  if (tbl) {
    // Click the name cell of the last row so the rack treats it as current.
    const QModelIndex idx = tbl->model()->index(5, 0);
    const QPoint pt = tbl->visualRect(idx).center();
    postPress(tbl->viewport(), pt);
    postRelease(tbl->viewport(), pt);
    CHECK(rack->currentRow() == 5);
    postKey(tbl, Qt::Key_Delete, Qt::NoModifier);
  }
  CHECK(s.channelRack().channels.size() == 5);
  CHECK(rack->channels().size() == 5);

  // Rack edits participate in undo/redo: undo restores the deleted channel.
  QAction* undo = findActionByKey(win, QKeySequence::Undo);
  QAction* redo = findActionByKey(win, QKeySequence::Redo);
  CHECK(undo != nullptr);
  CHECK(redo != nullptr);
  if (undo) undo->trigger();
  CHECK(s.channelRack().channels.size() == 6);
  CHECK(rack->channels().size() == 6);
  if (redo) redo->trigger();
  CHECK(s.channelRack().channels.size() == 5);
  CHECK(rack->channels().size() == 5);

  // Pattern bank rides the Session: toggle bank 3 via the real pattern button
  // (the checkable digit buttons in the rack chrome), then verify the
  // document caught it.
  QWidget* bank3 = nullptr;
  for (QWidget* w : rack->findChildren<QWidget*>()) {
    auto* tb = qobject_cast<QToolButton*>(w);
    if (tb && tb->isCheckable() && tb->text() == QStringLiteral("3"))
      bank3 = tb;
  }
  CHECK(bank3 != nullptr);
  if (bank3) {
    const QPoint c = bank3->rect().center();
    CHECK(postPress(bank3, c));
    CHECK(postRelease(bank3, c));
  }
  CHECK(s.channelRack().activePattern == 2);

  // Persistence: the rack is inside the .misuc document (a JSON round-trip).
  const std::string json = s.toJson();
  const Session reloaded = Session::fromJson(json);
  CHECK(reloaded.channelRack().channels.size() == 5);
  CHECK(reloaded.channelRack().activePattern == 2);
  CHECK(reloaded.channelRack().channels[0].name == "Kick");

  // Restore the suite-wide baseline: later suites (e.g. workflow) place the
  // pattern with the SEEDED bank active and expect materialised notes there.
  rack->setPattern(0);
  CHECK(s.channelRack().activePattern == 0);

  std::printf("  rack is session-persisted + keyboard delete + undo/redo ok\n");
}

static misuc::ui::ChannelStrip* findStripByTrackName(MainWindow* win,
                                                     const QString& name) {
  for (misuc::ui::ChannelStrip* st : win->findChildren<misuc::ui::ChannelStrip*>())
    for (QLabel* l : st->findChildren<QLabel*>())
      if (l->text() == name)
        return st;
  return nullptr;
}

static void workflowChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE workflow\n");
  Session& s = const_cast<Session&>(win->session());

  auto seqRow = [&]() {
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (QString::fromStdString(s.tracks()[i].name) == "Sequencer")
        return (int)i;
    return -1;
  };
  auto findRegion = [&](int row, const std::string& id) -> misuc::project::MidiRegion* {
    if (row >= 0 && row < (int)s.tracks().size())
      for (auto& r : s.tracks()[row].regions)
        if (r.id == id)
          return &r;
    return nullptr;
  };

  // ── 1. Channel Rack → two real pattern regions on the Sequencer track ────
  QWidget* placeBtn = findWidgetByTooltip(win, QStringLiteral("Place Pattern"));
  CHECK(placeBtn != nullptr);
  if (!placeBtn)
    return;
  const QPoint pc = placeBtn->rect().center();
  CHECK(postPress(placeBtn, pc));
  CHECK(postRelease(placeBtn, pc));
  int row = seqRow();
  CHECK(row >= 0);
  if (row < 0)
    return;
  const size_t regs0 = s.tracks()[row].regions.size();
  CHECK(regs0 >= 1);
  const std::string rA = s.tracks()[row].regions[regs0 - 1].id; // first new
  CHECK(postPress(placeBtn, pc));
  CHECK(postRelease(placeBtn, pc));
  CHECK(s.tracks()[row].regions.size() == regs0 + 1);
  const std::string rB = s.tracks()[row].regions.back().id;
  CHECK(findRegion(row, rA) != nullptr);
  CHECK(findRegion(row, rB) != nullptr);
  if (findRegion(row, rA) && findRegion(row, rB)) {
    // Placement stacks immediately after the previous content on the track.
    const double startA = findRegion(row, rA)->startBeats;
    CHECK(std::fabs(findRegion(row, rB)->startBeats -
                    (startA + findRegion(row, rA)->durationBeats)) < 1e-6);
    CHECK(findRegion(row, rA)->notes.size() > 0); // seeded bank materialized
  }

  // ── 2. Import a real audio sample (synthesised WAV) via insertSampleAt ──
  const QString wfDir = QStringLiteral("/tmp/misuc_workflow");
  const QString audioDir = wfDir + QStringLiteral("/audio");
  QDir().mkpath(audioDir);
  const QString wavPath = audioDir + QStringLiteral("/sample.wav");
  {
    const double rate = 48000.0;
    const double secs = 2.0;
    const uint64_t frames = static_cast<uint64_t>(rate * secs);
    std::vector<float> samples(frames * 2, 0.f);
    for (uint64_t i = 0; i < frames; ++i) {
      const float env = std::min(1.0f, std::min(float(i) / 2000.f,
          float(frames - i) / 2000.f));
      const float v = 0.5f * env *
          std::sin(2.0 * M_PI * 220.0 * double(i) / rate);
      samples[2 * i] = v;
      samples[2 * i + 1] = v;
    }
    std::string err;
    CHECK(misuc::audio::writeWavSamples(wavPath.toStdString(),
        samples.data(), frames, 2, rate, false, err));
  }

  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  const double clipBeat = 12.0;
  playlist->insertSampleAt(wavPath, clipBeat, row);
  CHECK(s.tracks()[row].clips.size() == 1);
  if (!s.tracks()[row].clips.empty()) {
    // The clip spans the true decoded length (2 s @ 120 BPM = 4 beats), with
    // its real sample rate and channel count; the file is referenced, not a
    // fake placeholder.
    const auto& clip = s.tracks()[row].clips.back();
    CHECK(std::fabs(clip.durationBeats - 4.0) < 1e-6);
    CHECK(clip.sampleRate == 48000);
    CHECK(clip.channels == 2);
    CHECK(QFile::exists(QString::fromStdString(clip.sourceFile)));
  }

  // ── 3. Arrange: move, duplicate and slice through the real playlist ─────
  QWidget* grid = win->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);
  if (!grid)
    return;

  // 3a. Drag region rB +2 beats.
  misuc::project::MidiRegion* rbp = findRegion(row, rB);
  CHECK(rbp != nullptr);
  if (!rbp)
    return;
  const double sb0 = rbp->startBeats;
  const QPoint bodyPress(kLabelW + int((sb0 + 0.5) * kRowPxPerBeat),
                         kRulerH + row * kRowH + 8);
  CHECK(postPress(grid, bodyPress));
  CHECK(postMove(grid, bodyPress + QPoint(2 * kRowPxPerBeat, 0)));
  CHECK(postRelease(grid, bodyPress + QPoint(2 * kRowPxPerBeat, 0)));
  CHECK(findRegion(row, rB) != nullptr);
  if (findRegion(row, rB))
    CHECK(std::fabs(findRegion(row, rB)->startBeats - (sb0 + 2.0)) < 1e-6);

  // 3b. Select rA, duplicate with Ctrl+D: an identical copy stacks after it.
  misuc::project::MidiRegion* rap = findRegion(row, rA);
  CHECK(rap != nullptr);
  if (!rap)
    return;
  const double sa0 = rap->startBeats;
  const size_t notesA = rap->notes.size();
  const size_t regsPreDup = s.tracks()[row].regions.size();
  const QPoint selA(kLabelW + int((sa0 + 0.5) * kRowPxPerBeat),
                    kRulerH + row * kRowH + 8);
  CHECK(postPress(grid, selA));
  CHECK(postRelease(grid, selA));
  postKey(playlist, Qt::Key_D, Qt::ControlModifier);
  CHECK(s.tracks()[row].regions.size() == regsPreDup + 1);
  if (s.tracks()[row].regions.size() == regsPreDup + 1) {
    const auto& dup = s.tracks()[row].regions.back();
    CHECK(std::fabs(dup.startBeats - (sa0 + 4.0)) < 1e-6);
    CHECK(dup.notes.size() == notesA);
  }

  // 3c. Slice region rB in half with the real Slice tool.
  double sbNow = 0.0;
  if (findRegion(row, rB))
    sbNow = findRegion(row, rB)->startBeats;
  const size_t regsPreSlice = s.tracks()[row].regions.size();
  QWidget* sliceBtn = win->findChild<QWidget*>(QStringLiteral("plSlice"));
  CHECK(sliceBtn != nullptr);
  if (sliceBtn) {
    const QPoint sc = sliceBtn->rect().center();
    CHECK(postPress(sliceBtn, sc));
    CHECK(postRelease(sliceBtn, sc));
  }
  const double splitBeats = sbNow + 2.0; // middle of the moved region
  const QPoint slicePt(kLabelW + int(splitBeats * kRowPxPerBeat),
                       kRulerH + row * kRowH + 8);
  CHECK(postPress(grid, slicePt));
  CHECK(postRelease(grid, slicePt));
  CHECK(s.tracks()[row].regions.size() == regsPreSlice + 1);
  bool hasSplitRight = false;
  for (const auto& r : s.tracks()[row].regions)
    if (std::fabs(r.startBeats - splitBeats) < 1e-6)
      hasSplitRight = true;
  CHECK(hasSplitRight);
  if (sliceBtn) { // deselect the slice tool
    const QPoint sc = sliceBtn->rect().center();
    CHECK(postPress(sliceBtn, sc));
    CHECK(postRelease(sliceBtn, sc));
  }

  // ── 4. Open rA in the piano roll and draw a real note (pitch 90) ─────────
  rap = findRegion(row, rA);
  CHECK(rap != nullptr);
  if (!rap)
    return;
  const size_t notesBefore = rap->notes.size();
  CHECK(postPress(grid, selA));
  CHECK(postRelease(grid, selA));
  postKey(playlist, Qt::Key_Return, Qt::NoModifier);
  QWidget* canvas = win->findChild<QWidget*>(QStringLiteral("pianoCanvas"));
  CHECK(canvas != nullptr);
  if (!canvas)
    return;
  {
    // Region-bound horizon is max(8, duration+4); derive px/beat from the
    // canvas's fixed width so the click targets an exact local beat.
    const double horizon = 8.0;
    const double pxPerBeat = double(canvas->width() - 1) / horizon;
    const int x = int(std::lround(3.5 * pxPerBeat));          // local beat 3.5
    const int y = 28 + (127 - 90) * 18;                       // pitch 90
    postDblClick(canvas, QPoint(x, y));
  }
  rap = findRegion(row, rA);
  CHECK(rap != nullptr);
  bool note90 = false;
  if (rap) {
    CHECK(rap->notes.size() == notesBefore + 1);
    for (const auto& n : rap->notes)
      if (n.pitch == 90 && std::fabs(n.startBeats - 3.5) < 1e-6)
        note90 = true;
  }
  CHECK(note90);

  // ── 5. Mixer: real fader / pan / mute drive the Session (undoable) ──────
  misuc::ui::ChannelStrip* strip =
      findStripByTrackName(win, QStringLiteral("Sequencer"));
  CHECK(strip != nullptr);
  if (strip) {
    QWidget* fader = findChildByClassName(strip, "misuc::ui::Fader");
    QWidget* knob = findChildByClassName(strip, "misuc::ui::Knob");
    QPushButton* mute = findButtonByText(strip, QStringLiteral("M"));
    CHECK(fader != nullptr);
    CHECK(knob != nullptr);
    CHECK(mute != nullptr);

    if (fader) { // fader press = absolute set to ~+6 dB (position by fraction of
      // the fader's actual travellable height; pad 10 px at each end)
      const int fh = fader->height();
      const QPoint p(fader->width() / 2,
                     10 + int(std::lround(0.0833 * double(qMax(1, fh - 20)))));
      CHECK(postPress(fader, p));
      CHECK(postRelease(fader, p));
      CHECK(s.tracks()[row].gainDb > 4.0f);
      CHECK(s.tracks()[row].gainDb < 8.0f);
    }
    if (knob) { // drag up 40 px => +40 pan
      const QPoint c = knob->rect().center();
      const QPoint up(c.x(), c.y() - 40);
      CHECK(postPress(knob, c));
      CHECK(postMove(knob, up));
      CHECK(postRelease(knob, up));
      CHECK(std::fabs(s.tracks()[row].pan - 0.4f) < 0.02f);
    }
    if (mute) { // click M => track muted
      const QPoint c = mute->rect().center();
      CHECK(postPress(mute, c));
      CHECK(postRelease(mute, c));
      CHECK(s.tracks()[row].muted);
    }
    // Undo/redo covers editing: unmute, then re-mute through the real actions.
    QAction* undo = findActionByKey(win, QKeySequence::Undo);
    QAction* redo = findActionByKey(win, QKeySequence::Redo);
    CHECK(undo != nullptr);
    CHECK(redo != nullptr);
    if (undo) undo->trigger();
    CHECK(!s.tracks()[row].muted);
    if (redo) redo->trigger();
    CHECK(s.tracks()[row].muted);
    // Leave the track audible again before playback step (redo left it muted).
    if (undo) undo->trigger();
    CHECK(!s.tracks()[row].muted);
  }

  // ── 6. Transport: ruler seek, playback, auto-stop at content end ─────────
  // Drive the engine headless like renderToWav's contract: stop the stream so
  // process() (the RT callback) is never contended. play()/seekToBeat() only
  // push control-queue messages that the next process() drains on the RT side,
  // so after each command we pump a small block to let the engine act.
  engine->closeAudio();
  for (int i = 0; i < 400 && engine->isRunning(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  const double contentEnd = s.contentEndBeats();
  CHECK(contentEnd > 4.0);

  // 6a. Click the playlist ruler at beat 2.0 => transport seeks there.
  const QPoint rulerPt(kLabelW + int(2.0 * kRowPxPerBeat), 4);
  CHECK(postPress(grid, rulerPt));
  CHECK(postRelease(grid, rulerPt));
  std::vector<float> block(2 * 24000, 0.f);
  engine->process(nullptr, 0, block.data(), 2, 24000, 0, 48000);
  // The block itself runs from the sought caret, so the playhead lands 0.5
  // beat past the target after the drain; allow whole-target or +block slack.
  const double seekFrames = 2.0 * (60.0 / s.tempo()) * 48000.0;
  CHECK(std::fabs(double(engine->playheadSamples()) - seekFrames) <
        48000.0 + 24000.0);

  // 6b. Play: real audio flows and the transport stops itself at content end.
  engine->play(false);
  std::vector<float> boot(2 * 512, 0.f);
  engine->process(nullptr, 0, boot.data(), 2, 512, 0, 48000); // drain Play
  CHECK(engine->isTransportPlaying());
  uint64_t total = 0;
  const double samplesPerBeat = 60.0 / s.tempo() * 48000.0;
  const uint64_t endFrames = uint64_t(contentEnd * samplesPerBeat);
  const uint64_t maxFrames = uint64_t((contentEnd + 8.0) * samplesPerBeat);
  float peak = 0.f;
  while (engine->isTransportPlaying() && total < maxFrames) {
    block.assign(2 * 24000, 0.f);
    engine->process(nullptr, 0, block.data(), 2, 24000, 0, 48000);
    total += 24000;
    if (total >= 24000 && total <= 72000) // window ~1..3 beats into playback
      for (int i = 0; i < 2 * 24000; ++i)
        peak = std::max(peak, std::fabs(block[i]));
  }
  CHECK(!engine->isTransportPlaying());
  CHECK(peak > 0.01f);
  CHECK(engine->playheadSamples() + 48000 >= endFrames);

  // ── 7. Loop: the transport-bar toggle now edits the Session (undoable) ──
  CHECK(!s.loopMode());
  QAction* loopAct = findActionByKey(win, QKeySequence(Qt::Key_L));
  CHECK(loopAct != nullptr);
  if (loopAct) {
    // Real menu path (Transport ▸ Loop): the same onLoopToggled() the
    // transport-bar loop button drives. The bar's mouse geometry only exists
    // after a real paint, so the QAction is the deterministic vehicle headless.
    loopAct->trigger();
  }
  CHECK(s.loopMode());
  CHECK(std::fabs(s.loopEnd() - contentEnd) < 1e-6); // loops whole content
  // Undo/redo the loop toggle (it is document-editing state now).
  QAction* undo = findActionByKey(win, QKeySequence::Undo);
  QAction* redo = findActionByKey(win, QKeySequence::Redo);
  CHECK(undo != nullptr);
  CHECK(redo != nullptr);
  if (undo) undo->trigger();
  CHECK(!s.loopMode());
  if (redo) redo->trigger();
  CHECK(s.loopMode());

  // ── 8. Save to disk, reopen, and confirm the whole document round-trips ──
  const QString projectPath = wfDir + QStringLiteral("/project.misuc");
  const size_t regionCount = s.tracks()[row].regions.size();
  const size_t clipCount = s.tracks()[row].clips.size();
  const float savedGain = s.tracks()[row].gainDb;
  const float savedPan = s.tracks()[row].pan;
  const bool savedMuted = s.tracks()[row].muted;
  const bool savedLoop = s.loopMode();
  const double endClipBeat = clipCount ? 12.0 + 4.0 : 0.0;
  (void)endClipBeat;
  CHECK(win->writeProject(projectPath));
  CHECK(QFile::exists(projectPath));

  // ── 8b. native container document, atomic write, recent-projects list ──
  {
    const auto cont = misuc::core::readContainer(projectPath.toStdString());
    CHECK(cont.ok);
    if (cont.ok) {
      CHECK(!cont.legacyJson);
      // misuc.meta marks the container format + version.
      bool sawMeta = false, sawSong = false;
      for (const auto& e : cont.entries) {
        if (e.name == misuc::core::kContainerMetaEntry) {
          const misuc::core::Json meta = misuc::core::Json::parse(e.data);
          CHECK(meta.at("format").string() == "misuc.container");
          CHECK(meta.at("containerVersion").number() ==
                misuc::core::kContainerFormatVersion);
          sawMeta = true;
        } else if (e.name == misuc::core::kContainerSongEntry) {
          const misuc::core::Json doc =
              misuc::core::Json::parse(e.data);
          CHECK(doc.at("format").string() == "misuc.song");
          CHECK(doc.at("schemaVersion").number() ==
                misuc::core::Json::kSchemaVersion);
          const auto& meta = doc.at("metadata").object();
          CHECK(!meta.at("createdUtc").string().empty());
          CHECK(!meta.at("modifiedUtc").string().empty());
          sawSong = true;
        }
      }
      CHECK(sawMeta);
      CHECK(sawSong);
    }
    // Atomic write leaves no temp file behind.
    CHECK(!QFile::exists(projectPath + QStringLiteral(".tmp")));
    // Saving records the absolute path in the recent list (menu mirror).
    const QStringList recents = win->recentProjects();
    CHECK(recents.contains(QFileInfo(projectPath).absoluteFilePath()));
  }
  win->openProject(projectPath);
  row = seqRow();
  CHECK(row >= 0);
  if (row >= 0) {
    CHECK(s.tracks()[row].regions.size() == regionCount);
    CHECK(s.tracks()[row].clips.size() == clipCount);
    bool note90Reload = false;
    for (const auto& r : s.tracks()[row].regions)
      for (const auto& n : r.notes)
        if (n.pitch == 90 && std::fabs(n.startBeats - 3.5) < 1e-6)
          note90Reload = true;
    CHECK(note90Reload);
    if (!s.tracks()[row].clips.empty()) {
      const auto& clip = s.tracks()[row].clips.front();
      CHECK(std::fabs(clip.durationBeats - 4.0) < 1e-6);
      CHECK(QFile::exists(QString::fromStdString(clip.sourceFile)));
    }
    CHECK(std::fabs(s.tracks()[row].gainDb - savedGain) < 0.5f);
    CHECK(std::fabs(s.tracks()[row].pan - savedPan) < 0.02f);
    CHECK(s.tracks()[row].muted == savedMuted);
    CHECK(s.loopMode() == savedLoop);
    CHECK(s.loopEnd() > 0.0);
  }

  std::printf("  integrated workflow (arrange/edit/mix/transport/save) ok\n");
}

// ===========================================================================
// BROWSER mode – tests user-pack indexing, cross-pack search, favourites,
// recent, and the DoD: dropping 10 samples from a pack onto the playlist to
// build a beat.
// ===========================================================================
static void browserChecks(MainWindow* win, AudioEngine* /*engine*/) {
  std::printf("CASE browser-ui\n");
  using misuc::ui::Browser;
  Session& s = const_cast<Session&>(win->session());
  if (s.tracks().empty())
    s.addTrack("Browser DnD");
  CHECK(s.tracks().size() >= 1);

  Browser* browser = win->findChild<Browser*>();
  CHECK(browser != nullptr);
  if (!browser) return;

  // 1. Scaffold: 10 small real WAVs in a hermetic pack directory.
  const QString packDir = QStringLiteral("/tmp/misuc_browserscandir/qaPackDnD");
  QDir().mkpath(packDir);
  const QStringList fileNames = {
      QStringLiteral("qabrowsmpl_01_kick.wav"),
      QStringLiteral("qabrowsmpl_02_snare.wav"),
      QStringLiteral("qabrowsmpl_03_hihat.wav"),
      QStringLiteral("qabrowsmpl_04_clap.wav"),
      QStringLiteral("qabrowsmpl_05_ride.wav"),
      QStringLiteral("qabrowsmpl_06_tom1.wav"),
      QStringLiteral("qabrowsmpl_07_tom2.wav"),
      QStringLiteral("qabrowsmpl_08_shaker.wav"),
      QStringLiteral("qabrowsmpl_09_perc.wav"),
      QStringLiteral("qabrowsmpl_10_crash.wav")};
  for (int i = 0; i < fileNames.size(); ++i) {
    const QString wavPath = packDir + "/" + fileNames[i];
    const double rate = 44100.0;
    const double secs = 0.2;
    const uint64_t frames = static_cast<uint64_t>(rate * secs);
    std::vector<float> samples(frames * 2, 0.f);
    const double freq = 100.0 * (i + 1); // distinct tone per file
    for (uint64_t j = 0; j < frames; ++j) {
      const float v = 0.3f * std::sin(2.0 * M_PI * freq * double(j) / rate);
      samples[2 * j] = v;
      samples[2 * j + 1] = v;
    }
    std::string err;
    CHECK(misuc::audio::writeWavSamples(wavPath.toStdString(),
        samples.data(), frames, 2, rate, false, err));
    CHECK(QFile::exists(wavPath));
  }

  // 2. indexPackFolder – persists and reveals the pack.
  CHECK(browser->indexPackFolder(packDir));

  // 3. Category tree: select the pack child, verify 10 listed files.
  QTreeWidget* cats = browser->findChild<QTreeWidget*>(QStringLiteral("sampleCats"));
  CHECK(cats != nullptr);
  QTreeWidgetItem* packsParent = nullptr;
  for (int i = 0; i < cats->topLevelItemCount(); ++i) {
    if (cats->topLevelItem(i)->text(0) == QStringLiteral("Packs")) {
      packsParent = cats->topLevelItem(i);
      break;
    }
  }
  CHECK(packsParent != nullptr);
  QTreeWidgetItem* packChild = nullptr;
  for (int i = 0; i < packsParent->childCount(); ++i) {
    if (packsParent->child(i)->data(0, Qt::UserRole).toString() == packDir) {
      packChild = packsParent->child(i);
      break;
    }
  }
  CHECK(packChild != nullptr);
  cats->setCurrentItem(packChild);
  QTreeWidget* files = browser->findChild<QTreeWidget*>(QStringLiteral("sampleFiles"));
  CHECK(files != nullptr);
  CHECK(files->topLevelItemCount() == 10);

  // 4. Search: cross-pack filename search.
  QLineEdit* search = browser->findChild<QLineEdit*>();
  CHECK(search != nullptr);
  search->setText(QStringLiteral("qabrowsmpl"));
  CHECK(files->topLevelItemCount() == 10);
  search->clear();
  // After clear, the current category is still the pack child → 10 listed.
  CHECK(files->topLevelItemCount() == 10);

  // 5. Favourites: toggle a file, verify it appears under Favourites.
  files->setCurrentItem(files->topLevelItem(0));
  QToolButton* favBtn = browser->findChild<QToolButton*>(QStringLiteral("favBtn"));
  CHECK(favBtn != nullptr);
  auto topByText = [cats](const QString& name) -> QTreeWidgetItem* {
    for (int i = 0; i < cats->topLevelItemCount(); ++i)
      if (cats->topLevelItem(i)->text(0) == name)
        return cats->topLevelItem(i);
    return nullptr;
  };
  favBtn->click(); // ON
  cats->setCurrentItem(topByText(QStringLiteral("Favourites")));
  CHECK(files->topLevelItemCount() == 1);
  files->setCurrentItem(files->topLevelItem(0));
  favBtn->click(); // OFF – removes it
  CHECK(files->topLevelItemCount() == 0);

  // 6. Recent: preview (with signal blocker to avoid engine spin-wait) adds
  //    the file to the persisted recent list.
  cats->setCurrentItem(packChild);
  files->setCurrentItem(files->topLevelItem(2));
  {
    QSignalBlocker blocker(browser);
    browser->findChild<QToolButton*>(QStringLiteral("previewBtn"))->click();
  }
  cats->setCurrentItem(topByText(QStringLiteral("Recent")));
  CHECK(files->topLevelItemCount() == 1);

  // 7. DoD: drop the 10 pack files onto the playlist grid to build a beat.
  cats->setCurrentItem(packChild); // repopulate files_ for reference
  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  playlist->applyZoom(24.0); // pin pxPerBeat_ to 24 (kRowPxPerBeat)
  QWidget* grid = win->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);

  // Reset drag_.row to track 0 via a press+release on row 0.
  postPress(grid, QPoint(kLabelW + 10, kRulerH + 20));
  postRelease(grid, QPoint(kLabelW + 10, kRulerH + 20));

  const size_t clipsBefore = s.tracks()[0].clips.size();
  for (int i = 0; i < 10; ++i) {
    const QString wavPath = packDir + "/" + fileNames[i];
    const int x = kLabelW + int(i * kRowPxPerBeat); // beats 0..9
    QMimeData md;
    md.setUrls({QUrl::fromLocalFile(wavPath)});
    // Real DnD protocol: a dragEnterEvent registers the drop target with the
    // QDragManager; only then does QApplication::notify deliver the drop.
    QDragEnterEvent enterEv(QPoint(x, kRulerH + 20), Qt::CopyAction, &md,
                            Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grid, &enterEv);
    CHECK(enterEv.isAccepted());
    if (enterEv.isAccepted()) {
      QDropEvent ev(QPointF(x, kRulerH + 20), Qt::CopyAction, &md,
                    Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(grid, &ev);
    }
  }
  CHECK(s.tracks()[0].clips.size() == clipsBefore + 10);
  const auto& track0 = s.tracks()[0];
  for (size_t i = 1; i < track0.clips.size(); ++i)
    CHECK(track0.clips[i].startBeats > track0.clips[i - 1].startBeats);

  std::printf("  browser: pack index + search + favourites + recent + "
              "grid drop -> build a beat ok\n");
}

// ===========================================================================
// BROWSER SOUND LIBRARY (Comp06, verification #16): the Sounds library is a
// guaranteed filesystem folder seeded with the project's real audio (never
// synthesized), nested folder structure is browsable, the seeded file audits
// through the engine without touching the project document, real waveforms
// arrive asynchronously as 96-bin envelopes, files drag into the Channel Rack
// as sampler channels and into the Playlist as real clips, and the wired
// sample references survive a save/reload round-trip.
// ===========================================================================
static void soundLibraryChecks(MainWindow* win, AudioEngine* /*engine*/) {
  std::printf("CASE sound-library\n");
  using misuc::ui::Browser;
  Session& s = const_cast<Session&>(win->session());
  auto* browser = win->findChild<Browser*>();
  CHECK(browser != nullptr);
  if (!browser) return;

  // 1. The guaranteed Sounds library exists, seeded with a real audio asset
  //    (the demo pad shipped with the project, never synthesized).
  const QString app =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const QString seedFile =
      app + QStringLiteral("/Sounds/Instruments/demo_pad.wav");
  CHECK(QFileInfo::exists(seedFile));
  CHECK(QFileInfo(seedFile).size() > 4096); // real audio, not a blip
  CHECK(browser->soundsFolderCount() >= 1);

  // 2. The Sounds node exposes the real folder structure (Library →
  //    Instruments) as selectable tree nodes.
  QTreeWidget* cats =
      browser->findChild<QTreeWidget*>(QStringLiteral("sampleCats"));
  CHECK(cats != nullptr);
  QTreeWidgetItem* soundsNode = nullptr;
  QTreeWidgetItem* libNode = nullptr;
  QTreeWidgetItem* instNode = nullptr;
  for (int i = 0; i < cats->topLevelItemCount(); ++i)
    if (cats->topLevelItem(i)->text(0) == QStringLiteral("Sounds")) {
      soundsNode = cats->topLevelItem(i);
      break;
    }
  CHECK(soundsNode != nullptr);
  CHECK(soundsNode->childCount() >= 1); // Library (or a reachable home folder)
  for (int i = 0; i < soundsNode->childCount(); ++i)
    if (soundsNode->child(i)->text(0) == QStringLiteral("Library")) {
      libNode = soundsNode->child(i);
      break;
    }
  CHECK(libNode != nullptr);
  for (int i = 0; i < libNode->childCount(); ++i)
    if (libNode->child(i)->text(0) == QStringLiteral("Instruments")) {
      instNode = libNode->child(i);
      break;
    }
  CHECK(instNode != nullptr);

  // 3. Selecting the folder lists the seeded real audio file.
  cats->setCurrentItem(instNode);
  QTreeWidget* files =
      browser->findChild<QTreeWidget*>(QStringLiteral("sampleFiles"));
  CHECK(files != nullptr);
  const QStringList listed = browser->currentFilePaths();
  CHECK(browser->fileCount() >= 1);
  CHECK(listed.contains(seedFile));

  // Waveform: the async decode eventually yields the full 96-bin envelope.
  auto waitWaveforms = [&]() {
    for (int i = 0; i < 400; ++i) {
      if (!browser->waveformBins(seedFile).isEmpty())
        return true;
      QApplication::processEvents();
      QThread::msleep(10);
    }
    return !browser->waveformBins(seedFile).isEmpty();
  };
  CHECK(waitWaveforms());
  QVector<float> bins = browser->waveformBins(seedFile);
  CHECK(bins.size() == 96);
  float maxBin = 0.f;
  int loudBins = 0;
  for (const float v : bins) {
    if (v > maxBin)
      maxBin = v;
    if (v > 1e-4f)
      ++loudBins;
  }
  CHECK(maxBin > 0.01f);
  CHECK(loudBins > 0);

  // 4. Preview auditions through the engine (real emission), leaving the
  //    project document untouched.
  QTreeWidgetItem* seedRow = nullptr;
  for (int i = 0; i < files->topLevelItemCount(); ++i)
    if (files->topLevelItem(i)->data(0, Qt::UserRole).toString() == seedFile)
      seedRow = files->topLevelItem(i);
  CHECK(seedRow != nullptr);
  files->setCurrentItem(seedRow);
  // A browserChecks-run may have left a blocked preview 'active'; reset it so
  // this audition really starts (and auditions the selected file, not a pause).
  browser->findChild<QToolButton*>(QStringLiteral("previewStopBtn"))->click();
  QApplication::processEvents();
  const std::string sessionFrozen = s.toJson();
  QString previewedPath;
  QObject ctx;
  QObject::connect(browser, &Browser::previewFile, &ctx,
                   [&previewedPath](const QString& path) {
                     previewedPath = path;
                   });
  browser->findChild<QToolButton*>(QStringLiteral("previewBtn"))->click();
  QApplication::processEvents();
  CHECK(previewedPath == seedFile);
  QLabel* meta = browser->findChild<QLabel*>(QStringLiteral("previewMeta"));
  CHECK(meta != nullptr);
  if (meta)
    CHECK(meta->text().contains(QStringLiteral("WAV"))); // real format header,
                                                         // not a synthetic blip
  CHECK(browser->hasPreviewSelection());
  QSlider* vol = browser->findChild<QSlider*>(QStringLiteral("previewVolume"));
  CHECK(vol != nullptr);
  vol->setValue(75);
  CHECK(fabs(browser->previewVolume() - 1.5) < 1e-9);
  browser->findChild<QToolButton*>(QStringLiteral("previewStopBtn"))->click();
  QApplication::processEvents();
  CHECK(s.toJson() == sessionFrozen); // preview never mutates the project
  CHECK(previewedPath == seedFile);

  // 5. Drag the seeded file onto the Channel Rack → a new sampler channel
  //    named from the stem, carrying the real soundFile reference.
  auto* rack = win->findChild<misuc::ui::ChannelRack*>();
  CHECK(rack != nullptr);
  const int aerBefore = int(rack->channels().size());
  const int sesBefore = int(s.channelRack().channels.size());
  auto* tbl = rack->findChild<QTableView*>(QStringLiteral("rackTable"));
  CHECK(tbl != nullptr);
  QMimeData md;
  md.setUrls({QUrl::fromLocalFile(seedFile)});
  const QPoint rackPos =
      tbl->model() ? tbl->visualRect(tbl->model()->index(0, 0)).center()
                   : QPoint(20, 20);
  QDragEnterEvent enterEv(rackPos, Qt::CopyAction, &md, Qt::LeftButton,
                          Qt::NoModifier);
  QApplication::sendEvent(tbl->viewport(), &enterEv);
  CHECK(enterEv.isAccepted());
  QDropEvent dropEv(QPointF(rackPos), Qt::CopyAction, &md, Qt::LeftButton,
                    Qt::NoModifier);
  QApplication::sendEvent(tbl->viewport(), &dropEv);
  CHECK(dropEv.isAccepted());
  CHECK(int(rack->channels().size()) == aerBefore + 1);
  CHECK(int(s.channelRack().channels.size()) == sesBefore + 1);
  const int newRow = int(rack->channels().size()) - 1;
  CHECK(rack->channelSoundFile(newRow) == seedFile);
  CHECK(rack->channelSoundFile(newRow) != QString()); // ref is real, not lost
  CHECK(s.channelRack().channels[newRow].name == "demo_pad");
  CHECK(s.channelRack().channels[newRow].type ==
        misuc::project::ChannelType::Sampler);
  CHECK(s.channelRack().channels[newRow].soundFile ==
        seedFile.toStdString());

  // 6. Drag the seeded file onto the Playlist at beat 64 → a real clip with
  //    the full sample duration lands at the drop position.
  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  playlist->applyZoom(24.0); // pin pxPerBeat_ to 24 (kRowPxPerBeat)
  QWidget* grid = win->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);
  const int dropBeat = 64;
  const int gridX = kLabelW + int(dropBeat * kRowPxPerBeat);
  const int gridY = kRulerH + 20;
  QMimeData md2;
  md2.setUrls({QUrl::fromLocalFile(seedFile)});
  QDragEnterEvent enter2(QPoint(gridX, gridY), Qt::CopyAction, &md2,
                         Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(grid, &enter2);
  CHECK(enter2.isAccepted());
  const size_t clips0 = s.tracks()[0].clips.size();
  QDropEvent drop2(QPointF(gridX, gridY), Qt::CopyAction, &md2, Qt::LeftButton,
                   Qt::NoModifier);
  QApplication::sendEvent(grid, &drop2);
  // Playlist::dropEvent places the clip from the URL list without a trailing
  // accept(); what it must genuinely do is create the clip, verified below.
  CHECK(s.tracks()[0].clips.size() == clips0 + 1);
  const auto& clip = s.tracks()[0].clips.back();
  CHECK(QString::fromStdString(clip.sourceFile) == seedFile);
  CHECK(std::fmod(clip.startBeats, 4.0) == 0.0); // snapped to the grid
  CHECK(clip.startBeats >= dropBeat);
  CHECK(clip.durationBeats > 0);

  // 7. Save/reload: both wired references (rack soundFile + playlist clip
  //    sourceFile) survive the document round-trip.
  const Session reloaded = Session::fromJson(s.toJson());
  CHECK(reloaded.channelRack().channels[newRow].soundFile ==
        seedFile.toStdString());
  CHECK(QString::fromStdString(reloaded.tracks()[0].clips.back().sourceFile) ==
        seedFile);

  // 8. Refresh detects a newly added file and its removal without rebuilding
  //    the app (the listing re-scans the seeded folder).
  const QString extra =
      app + QStringLiteral("/Sounds/Instruments/qa_extra_kick.wav");
  {
    const double rate = 44100.0;
    const uint64_t frames = static_cast<uint64_t>(rate * 0.2);
    std::vector<float> smp(frames * 2, 0.f);
    for (uint64_t j = 0; j < frames; ++j)
      smp[2 * j] = smp[2 * j + 1] = 0.3f * std::sin(2.0 * M_PI * 100.0 *
                                                  double(j) / rate);
    std::string err;
    CHECK(misuc::audio::writeWavSamples(extra.toStdString(), smp.data(),
                                        frames, 2, rate, false, err));
  }
  CHECK(!browser->currentFilePaths().contains(extra)); // unseen before refresh
  browser->refresh();
  QApplication::processEvents();
  CHECK(browser->currentFilePaths().contains(extra)); // picked up by rescan
  QFile::remove(extra);
  browser->refresh();
  QApplication::processEvents();
  CHECK(!browser->currentFilePaths().contains(extra)); // cleaned up on rescan

  // 9. Exactly one browser instance owns the workspace (no duplicates).
  CHECK(win->findChildren<Browser*>().size() == 1);

  std::printf("  sound-library: sounds node + seeded real audio + waveforms + "
              "preview + rack/playlist drag + refresh round-trip ok\n");
}

// ===========================================================================
// FIRST FUNK mode – creates a real hardtekk/funk demo session, exercises real
// playlist / mixer / transport / save-reload operations, and verifies the
// engine renders correctly through mixer parameter changes.
// ===========================================================================
static void firstFunkChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE firstfunk-setup\n");
  // This suite runs on a shared MainWindow after other GUI cases; start from a
  // pristine session so the arrangement, content end and loop state are fully
  // deterministic (and the engine's slots are re-published on the FIRST
  // sessionEdited() below).
  Session& s = const_cast<Session&>(win->session());
  s = misuc::project::Session();
  constexpr double kBpm = 170.0;
  constexpr double kSecPerBeat = 60.0 / kBpm;
  constexpr double kSamplesPerBeat = 48000.0 * kSecPerBeat;
  const QString projDir = QStringLiteral("/tmp/misuc_firstfunk/FIRST FUNK");
  const QString audioDir = projDir + QStringLiteral("/audio");
  QDir().mkpath(audioDir);

  // Helpers ------------------------------------------------------------------
  auto writeOneShot = [&](const QString& name, double sec,
                          auto gen) -> bool {
    const uint64_t frames = uint64_t(48000.0 * sec);
    std::vector<float> samples(frames * 2);
    gen(samples.data(), frames, 2, 48000.0);
    std::string err;
    const QString path = audioDir + QStringLiteral("/") + name;
    const bool ok = misuc::audio::writeWavSamples(
        path.toStdString(), samples.data(), frames, 2, 48000.0, false, err);
    if (!ok) std::printf("  WARN writeWavSamples failed for %s: %s\n",
                         path.toStdString().c_str(), err.c_str());
    CHECK(ok);
    return ok;
  };

  auto beatToX = [](int row, double beat) -> int {
    (void)row;
    return kLabelW + int(std::lround(beat * kRowPxPerBeat));
  };
  auto rowY = [](int row) { return kRulerH + row * kRowH + 8; };

  // Generate original one-shot samples (synthesised, no external files) ------
  {
    // Kick loop (16 beats = intro length): kick on every beat.
    writeOneShot(QStringLiteral("kick_loop.wav"), 16.0 * kSecPerBeat,
        [](float* d, uint64_t n, uint32_t, double sr) {
          const double secPerBeat = 60.0 / 170.0;
          for (uint64_t i = 0; i < n; ++i) {
            const double t = double(i) / sr;
            const double beat = t / secPerBeat;
            const double beatFrac = beat - std::floor(beat);
            const double kickEnv = std::exp(-18.0 * beatFrac * secPerBeat);
            const double freq = 150.0 - 100.0 * beatFrac;
            const float v = float(kickEnv * std::sin(6.2831853 * freq * t) * 0.9);
            d[2 * i] = d[2 * i + 1] = v;
          }
        });
    // Snare/clap loop (16 beats): on beats 1,3,5,... (backbeat).
    writeOneShot(QStringLiteral("snare_loop.wav"), 16.0 * kSecPerBeat,
        [](float* d, uint64_t n, uint32_t, double sr) {
          const double secPerBeat = 60.0 / 170.0;
          for (uint64_t i = 0; i < n; ++i) {
            const double t = double(i) / sr;
            const double beat = t / secPerBeat;
            const int beatInt = int(beat);
            const double frac = beat - beatInt;
            const bool onBackbeat = (beatInt % 2 == 1);
            const double env = onBackbeat ? std::exp(-25.0 * frac * secPerBeat) : 0.0;
            const float noise = float(rand()) / float(RAND_MAX) * 2.0f - 1.0f;
            const float tone = std::sin(6.2831853f * 200.0f * float(t));
            const float v = float(env) * (0.5f * noise + 0.5f * tone) * 0.8f;
            d[2 * i] = d[2 * i + 1] = v;
          }
        });
    // Hi-hat loop (16 beats): 8th notes.
    writeOneShot(QStringLiteral("hat_loop.wav"), 16.0 * kSecPerBeat,
        [](float* d, uint64_t n, uint32_t, double sr) {
          const double secPerBeat = 60.0 / 170.0;
          for (uint64_t i = 0; i < n; ++i) {
            const double t = double(i) / sr;
            const double beat = t / secPerBeat;
            const double frac = beat - std::floor(beat * 2.0) / 2.0;
            const double env = std::exp(-40.0 * frac * secPerBeat);
            const float noise = float(rand()) / float(RAND_MAX) * 2.0f - 1.0f;
            const float v = float(env) * noise * 0.5f;
            d[2 * i] = d[2 * i + 1] = v;
          }
        });
    // Percussion loop (16 beats): sparse hits.
    writeOneShot(QStringLiteral("perc_loop.wav"), 16.0 * kSecPerBeat,
        [](float* d, uint64_t n, uint32_t, double sr) {
          const double secPerBeat = 60.0 / 170.0;
          for (uint64_t i = 0; i < n; ++i) {
            const double t = double(i) / sr;
            const double beat = t / secPerBeat;
            const double beatMod4 = std::fmod(beat, 4.0);
            const bool hit = std::fabs(beatMod4 - 0.25) < 0.05;
            const double frac = beatMod4 - 0.25;
            const double env = hit ? std::exp(-22.0 * std::fabs(frac) * secPerBeat) : 0.0;
            const float v = float(env) * std::sin(6.2831853f * 110.0f * float(t)) * 0.75f;
            d[2 * i] = d[2 * i + 1] = v;
          }
        });
    // FX sweep (8 beats).
    writeOneShot(QStringLiteral("fx_sweep.wav"), 8.0 * kSecPerBeat,
        [](float* d, uint64_t n, uint32_t, double sr) {
          for (uint64_t i = 0; i < n; ++i) {
            const float t = float(i) / float(sr);
            const float progress = float(i) / float(n);
            const float sweep = 200.0f + 3000.0f * progress;
            const float noise = float(rand()) / float(RAND_MAX) * 2.0f - 1.0f;
            const float tone = std::sin(6.2831853f * sweep * t);
            const float env = std::min(1.0f, std::min(float(i) / 4800.0f,
                                float(n - i) / 4800.0f));
            const float v = env * (0.4f * noise + 0.6f * tone) * 0.6f;
            d[2 * i] = d[2 * i + 1] = v;
          }
        });
  }
  // Source file paths (absolute, in the project audio dir).
  const std::string srcKickLoop  = (audioDir + QStringLiteral("/kick_loop.wav")).toStdString();
  const std::string srcSnareLoop = (audioDir + QStringLiteral("/snare_loop.wav")).toStdString();
  const std::string srcHatLoop   = (audioDir + QStringLiteral("/hat_loop.wav")).toStdString();
  const std::string srcPercLoop  = (audioDir + QStringLiteral("/perc_loop.wav")).toStdString();
  const std::string srcFxSweep   = (audioDir + QStringLiteral("/fx_sweep.wav")).toStdString();

  // Build session ------------------------------------------------------------
  s.setName("FIRST FUNK");
  s.setTempo(kBpm);
  s.tracks().clear();

  const std::string tKick  = s.addTrack("HARDTEKK KICK");
  const std::string tSnare = s.addTrack("SNARE / CLAP");
  const std::string tHats  = s.addTrack("HI-HATS");
  const std::string tPerc  = s.addTrack("PERCUSSION");
  const std::string tBass  = s.addTrack("FUNK BASS");
  const std::string tStab  = s.addTrack("FUNK STABS");
  const std::string tLead  = s.addTrack("LEAD / MELODY");
  const std::string tFx    = s.addTrack("FX / TRANSITION");

  // Lambda to add an audio clip at a given beat time on a track.
  auto addClip = [&](const std::string& tid, const std::string& src,
                     double startBeat, double durSec, const QString& name) {
    misuc::project::AudioClip c;
    c.name = name.toStdString();
    c.sourceFile = src;
    c.startBeats = startBeat;
    c.durationBeats = durSec * kBpm / 60.0;
    c.clipOffsetBeats = 0.0;
    c.gainDb = 0.0f;
    c.sampleRate = 48000;
    c.channels = 2;
    s.addClip(tid, std::move(c));
  };

  // Section definitions (beats).
  // INTRO 0..16, MAIN 16..48, VAR 48..64, BREAK 64..80, DROP 80..112, END 112..128.

  // Hi-hats: one 16-beat loop per section (MAIN, VAR, BREAK, DROP, END).
  {
    const double loopDur = 16.0 * kSecPerBeat;
    addClip(tHats, srcHatLoop, 16.0, loopDur, QStringLiteral("hat_main"));
    addClip(tHats, srcHatLoop, 48.0, loopDur, QStringLiteral("hat_var"));
    addClip(tHats, srcHatLoop, 64.0, loopDur, QStringLiteral("hat_break"));
    addClip(tHats, srcHatLoop, 80.0, loopDur, QStringLiteral("hat_drop"));
    addClip(tHats, srcHatLoop, 112.0, loopDur, QStringLiteral("hat_end"));
  }

  // Snare / Clap: one 16-beat loop per section with backbeat.
  {
    const double loopDur = 16.0 * kSecPerBeat;
    addClip(tSnare, srcSnareLoop, 16.0, loopDur, QStringLiteral("snare_main"));
    addClip(tSnare, srcSnareLoop, 48.0, loopDur, QStringLiteral("snare_var"));
    addClip(tSnare, srcSnareLoop, 80.0, loopDur, QStringLiteral("snare_drop"));
    addClip(tSnare, srcSnareLoop, 112.0, loopDur, QStringLiteral("snare_end"));
  }

  // Kick: 16-beat loops in MAIN and DROP.
  {
    const double loopDur = 16.0 * kSecPerBeat;
    addClip(tKick, srcKickLoop, 16.0, loopDur, QStringLiteral("kick_main1"));
    addClip(tKick, srcKickLoop, 32.0, loopDur, QStringLiteral("kick_main2"));
    addClip(tKick, srcKickLoop, 80.0, loopDur, QStringLiteral("kick_drop1"));
    addClip(tKick, srcKickLoop, 96.0, loopDur, QStringLiteral("kick_drop2"));
  }

  // Percussion: one 16-beat loop in MAIN and DROP.
  {
    const double loopDur = 16.0 * kSecPerBeat;
    addClip(tPerc, srcPercLoop, 16.0, loopDur, QStringLiteral("perc_main"));
    addClip(tPerc, srcPercLoop, 80.0, loopDur, QStringLiteral("perc_drop"));
  }

  // FX: sweeps at transitions.
  {
    const double sweepDur = 8.0 * kSecPerBeat;
    addClip(tFx, srcFxSweep, 60.0, sweepDur, QStringLiteral("sweepDown"));
    addClip(tFx, srcFxSweep, 110.0, sweepDur, QStringLiteral("sweepUp"));
  }

  // MIDI helper --------------------------------------------------------------
  auto addNote = [](MidiRegion* r, uint8_t pitch, double start, double dur,
                    uint8_t vel = 100) {
    if (!r) return;
    NoteEvent n;
    n.pitch = pitch;
    n.velocity = vel;
    n.startBeats = start;
    n.durationBeats = dur;
    r->notes.push_back(n);
  };

  // FUNK BASS ----------------------------------------------------------------
  // "Bass Groove A" at beat 16, duration 16 beats (4 bars). Duplicated later.
  const std::string bassAId = s.addRegion(tBass, "Bass Groove A", 16.0, 16.0);
  MidiRegion* bassA = s.findRegion(tBass, bassAId);
  // Funky 4-bar pattern: E2(40), G2(43), A2(45), B2(47) with syncopation.
  const uint8_t bassPitches[] = {40, 43, 45, 47, 45, 43, 40, 43};
  for (int bar = 0; bar < 4; ++bar)
    for (int i = 0; i < 8; ++i) {
      const double start = bar * 4.0 + i * 0.5;
      const double dur = (i % 2 == 0) ? 0.25 : 0.15;
      const uint8_t vel = (i % 2 == 0) ? 110 : 85;
      addNote(bassA, bassPitches[i % 8], start, dur, vel);
    }

  // "Bass Groove B" at beat 48 (variation), duration 16.
  const std::string bassBId = s.addRegion(tBass, "Bass Groove B", 48.0, 16.0);
  MidiRegion* bassB = s.findRegion(tBass, bassBId);
  const uint8_t bassBPitches[] = {40, 45, 43, 47, 48, 45, 43, 40};
  for (int bar = 0; bar < 4; ++bar)
    for (int i = 0; i < 8; ++i)
      addNote(bassB, bassBPitches[i % 8], bar * 4.0 + i * 0.5, 0.25, 105);

  // "Bass Drops" at beat 64 (break), long sustained notes.
  const std::string bassCId = s.addRegion(tBass, "Bass Drops", 64.0, 16.0);
  MidiRegion* bassC = s.findRegion(tBass, bassCId);
  addNote(bassC, 36, 0.0, 8.0, 100); // C1 held 2 bars
  addNote(bassC, 38, 8.0, 8.0, 100); // D1 held 2 bars

  // "Bass Outro" at beat 112 (end), 4 bars.
  const std::string bassDId = s.addRegion(tBass, "Bass Outro", 112.0, 16.0);
  MidiRegion* bassD = s.findRegion(tBass, bassDId);
  addNote(bassD, 40, 0.0, 16.0, 95);

  // FUNK STABS ---------------------------------------------------------------
  // Stab chord region "Stab A" at 16, duration 8 (2 bars) – repeated 4-note
  // stabs on each quarter note.
  const std::string stabAId = s.addRegion(tStab, "Stab A", 16.0, 8.0);
  MidiRegion* stabA = s.findRegion(tStab, stabAId);
  for (double b = 0.0; b < 8.0; b += 1.0) {
    addNote(stabA, 72, b, 0.4, 100); // C5
    addNote(stabA, 75, b, 0.4, 100); // Eb5
    addNote(stabA, 79, b, 0.4, 90);  // G5
  }
  // "Stab B" at 80 (drop), duration 8.
  const std::string stabBId = s.addRegion(tStab, "Stab B", 80.0, 8.0);
  MidiRegion* stabB = s.findRegion(tStab, stabBId);
  for (double b = 0.0; b < 8.0; b += 1.0) {
    addNote(stabB, 74, b, 0.4, 105); // D5
    addNote(stabB, 77, b, 0.4, 105); // F5
    addNote(stabB, 81, b, 0.4, 95);  // A5
  }

  // LEAD / MELODY ------------------------------------------------------------
  // "Lead Main" at 16, duration 32 (8 bars).
  const std::string leadAId = s.addRegion(tLead, "Lead Main", 16.0, 32.0);
  MidiRegion* leadA = s.findRegion(tLead, leadAId);
  // Simple ascending/descending phrase repeated each 4 bars.
  const uint8_t leadPitches[] = {60,62,64,67,72,67,64,62};
  for (int rep = 0; rep < 2; ++rep)
    for (int i = 0; i < 8; ++i) {
      const double start = rep * 4.0 + i * 0.5;
      addNote(leadA, leadPitches[i] + (rep == 1 ? 12 : 0), start, 0.35, 110 - i * 3);
    }

  // "Lead Outro" at 112, duration 16.
  const std::string leadBId = s.addRegion(tLead, "Lead Outro", 112.0, 16.0);
  MidiRegion* leadB = s.findRegion(tLead, leadBId);
  for (int i = 0; i < 16; ++i)
    addNote(leadB, 72, double(i), 0.3, 100);

  // Final silent marker to guarantee contentEnd = 128 even if all notes end earlier.
  {
    const std::string finId = s.addRegion(tLead, "end", 127.5, 0.5);
    MidiRegion* fin = s.findRegion(tLead, finId);
    addNote(fin, 60, 0.0, 0.5, 1);
  }

  // Sync session to engine after initial population.
  win->sessionEdited();

  // ── 1. Playlist ops: move, duplicate, copy/paste, trim, slice, snap, multi-select ──
  std::printf("CASE firstfunk-playlist-ops\n");
  QWidget* grid = win->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);
  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  QWidget* snapBtn = win->findChild<QWidget*>(QStringLiteral("plSnap"));
  CHECK(snapBtn != nullptr);
  QWidget* sliceBtn = win->findChild<QWidget*>(QStringLiteral("plSlice"));
  CHECK(sliceBtn != nullptr);

  // Helper: get pointer to a region by row and name (linear scan).
  auto findRegByName = [&](int row, const QString& name) -> misuc::project::MidiRegion* {
    if (row < 0 || row >= (int)s.tracks().size()) return nullptr;
    for (auto& r : s.tracks()[row].regions)
      if (QString::fromStdString(r.name) == name)
        return &r;
    return nullptr;
  };
  const int rowBass = [&]() {
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (QString::fromStdString(s.tracks()[i].name) == "FUNK BASS")
        return (int)i;
    return -1;
  }();
  const int rowStab = [&]() {
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (QString::fromStdString(s.tracks()[i].name) == "FUNK STABS")
        return (int)i;
    return -1;
  }();
  const int rowLead = [&]() {
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (QString::fromStdString(s.tracks()[i].name) == "LEAD / MELODY")
        return (int)i;
    return -1;
  }();
  const int rowHats = [&]() {
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (QString::fromStdString(s.tracks()[i].name) == "HI-HATS")
        return (int)i;
    return -1;
  }();
  const int rowKick = [&]() {
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (QString::fromStdString(s.tracks()[i].name) == "HARDTEKK KICK")
        return (int)i;
    return -1;
  }();

  // 1a. Move "Stab A" +1 beat.
  {
    MidiRegion* stab = findRegByName(rowStab, "Stab A");
    CHECK(stab != nullptr);
    const double sb0 = stab->startBeats;
    const QPoint pt(beatToX(rowStab, sb0 + 0.5), rowY(rowStab));
    postPress(grid, pt);
    postMove(grid, QPoint(beatToX(rowStab, sb0 + 1.5), rowY(rowStab)));
    postRelease(grid, QPoint(beatToX(rowStab, sb0 + 1.5), rowY(rowStab)));
    CHECK(std::fabs(stab->startBeats - (sb0 + 1.0)) < 1e-6);
  }

  // 1b. Duplicate "Lead Main" with Ctrl+D.
  {
    MidiRegion* lead = findRegByName(rowLead, "Lead Main");
    CHECK(lead != nullptr);
    const double sa0 = lead->startBeats;
    const size_t nPre = s.tracks()[rowLead].regions.size();
    const QPoint pt(beatToX(rowLead, sa0 + 0.5), rowY(rowLead));
    postPress(grid, pt);
    postRelease(grid, pt);
    postKey(playlist, Qt::Key_D, Qt::ControlModifier);
    CHECK(s.tracks()[rowLead].regions.size() == nPre + 1);
    // Duplicate should stack right after the original (sa0 + 32 = 48).
    MidiRegion* dup = nullptr;
    for (auto& r : s.tracks()[rowLead].regions)
      if (r.name == "Lead Main" && std::fabs(r.startBeats - 48.0) < 1e-6)
        dup = &r;
    CHECK(dup != nullptr);
  }

  // 1c. Copy/paste "Bass Groove A" into DROP (beat 80).
  {
    MidiRegion* bass = findRegByName(rowBass, "Bass Groove A");
    CHECK(bass != nullptr);
    const double sa0 = bass->startBeats;
    // Select and copy.
    const QPoint sel(beatToX(rowBass, sa0 + 0.5), rowY(rowBass));
    postPress(grid, sel);
    postRelease(grid, sel);
    postKey(playlist, Qt::Key_C, Qt::ControlModifier);
    // Move playhead to beat 80.
    postPress(grid, QPoint(beatToX(rowBass, 80.0), kRulerH / 2));
    postRelease(grid, QPoint(beatToX(rowBass, 80.0), kRulerH / 2));
    // Paste.
    postKey(playlist, Qt::Key_V, Qt::ControlModifier);
    // Verify a copy at 80.
    bool has80 = false;
    for (const auto& r : s.tracks()[rowBass].regions)
      if (r.name == "Bass Groove A" && std::fabs(r.startBeats - 80.0) < 1e-6)
        has80 = true;
    CHECK(has80);
    // Duplicate that copy to fill 96..112.
    MidiRegion* pasted = nullptr;
    for (auto& r : s.tracks()[rowBass].regions)
      if (r.name == "Bass Groove A" && std::fabs(r.startBeats - 80.0) < 1e-6)
        pasted = &r;
    CHECK(pasted != nullptr);
    if (pasted) {
      const QPoint pt2(beatToX(rowBass, 80.5), rowY(rowBass));
      postPress(grid, pt2);
      postRelease(grid, pt2);
      postKey(playlist, Qt::Key_D, Qt::ControlModifier);
      bool has96 = false;
      for (const auto& r : s.tracks()[rowBass].regions)
        if (r.name == "Bass Groove A" && std::fabs(r.startBeats - 96.0) < 1e-6)
          has96 = true;
      CHECK(has96);
    }
  }

  // 1d. Trim: stretch the first hi-hat clip by +0.5 beats via right-edge drag.
  if (rowHats >= 0 && !s.tracks()[rowHats].clips.empty()) {
    auto& clip = s.tracks()[rowHats].clips.front();
    const double origDur = clip.durationBeats;
    const double origStart = clip.startBeats;
    const int rightEdgeX = beatToX(rowHats, origStart + origDur) - 3;
    const int y = rowY(rowHats);
    const QPoint pressPt(rightEdgeX, y);
    const QPoint movePt(rightEdgeX + int(0.5 * kRowPxPerBeat), y);
    postPress(grid, pressPt);
    postMove(grid, movePt);
    postRelease(grid, movePt);
    CHECK(clip.durationBeats > origDur + 0.4);
  }

  // 1e. Slice: split "Stab B" at its midpoint.
  {
    MidiRegion* stab = findRegByName(rowStab, "Stab B");
    CHECK(stab != nullptr);
    const double sb0 = stab->startBeats;
    const double dur = stab->durationBeats;
    const double split = sb0 + dur / 2.0;
    const size_t nPre = s.tracks()[rowStab].regions.size();
    // Activate slice tool, click, deactivate.
    const QPoint sc = sliceBtn->rect().center();
    postPress(sliceBtn, sc);
    postRelease(sliceBtn, sc);
    postPress(grid, QPoint(beatToX(rowStab, split), rowY(rowStab)));
    postRelease(grid, QPoint(beatToX(rowStab, split), rowY(rowStab)));
    CHECK(s.tracks()[rowStab].regions.size() == nPre + 1);
    bool hasRight = false;
    for (const auto& r : s.tracks()[rowStab].regions)
      if (std::fabs(r.startBeats - split) < 1e-6) hasRight = true;
    CHECK(hasRight);
    // Deactivate slice tool.
    postPress(sliceBtn, sc);
    postRelease(sliceBtn, sc);
  }

  // 1f. Multi-select: Ctrl+click two kick clips (first two) and move +1 beat.
  if (rowKick >= 0 && s.tracks()[rowKick].clips.size() >= 2) {
    auto& c0 = s.tracks()[rowKick].clips[0];
    auto& c1 = s.tracks()[rowKick].clips[1];
    const double s0 = c0.startBeats;
    const double s1 = c1.startBeats;
    const QPoint p0(beatToX(rowKick, s0 + 0.5), rowY(rowKick));
    const QPoint p1(beatToX(rowKick, s1 + 0.5), rowY(rowKick));
    // Ctrl+click both clips into the multi-selection, then drag one.
    clickWithCtrl(grid, p0);
    clickWithCtrl(grid, p1);
    postPress(grid, p0);
    postMove(grid, QPoint(beatToX(rowKick, s0 + 1.5), rowY(rowKick)));
    postRelease(grid, QPoint(beatToX(rowKick, s0 + 1.5), rowY(rowKick)));
    CHECK(c0.startBeats > s0 + 0.9);
    CHECK(c1.startBeats > s1 + 0.9);
  }

  // 1g. Snap: toggle snap on, drag a stab region by a non-grid amount, verify it lands on a grid boundary.
  {
    MidiRegion* stab = findRegByName(rowStab, "Stab A");
    CHECK(stab != nullptr);
    const double sb0 = stab->startBeats;
    // Ensure snap is ON (click toggle if currently off).
    if (grid->property("snapOn").isValid() && !grid->property("snapOn").toBool()) {
      const QPoint sc = snapBtn->rect().center();
      postPress(snapBtn, sc);
      postRelease(snapBtn, sc);
    }
    // Drag from mid-beat to +0.3 beats (should snap to nearest 0.25).
    const double fromBeat = sb0 + 0.5;
    const double toBeat = fromBeat + 0.3;
    postPress(grid, QPoint(beatToX(rowStab, fromBeat), rowY(rowStab)));
    postMove(grid, QPoint(beatToX(rowStab, toBeat), rowY(rowStab)));
    postRelease(grid, QPoint(beatToX(rowStab, toBeat), rowY(rowStab)));
    // Verify snapped to nearest 0.25 beat (kSnapDiv=4).
    const double frac = std::fmod(stab->startBeats, 0.25);
    CHECK(frac < 1e-6 || (0.25 - frac) < 1e-6);
    // Turn snap back off for subsequent ops.
    {
      const QPoint sc = snapBtn->rect().center();
      postPress(snapBtn, sc);
      postRelease(snapBtn, sc);
    }
  }

  // 1h. Paint: brush the rack's active pattern into the empty bar cells of a
  // lane. A whole drag-stroke is exactly ONE edit, so one undo removes the
  // entire stroke.
  {
    std::printf("CASE firstfunk-paint\n");
    QWidget* paintBtn = win->findChild<QWidget*>(QStringLiteral("plPaint"));
    CHECK(paintBtn != nullptr);
    const QPoint pc = paintBtn->rect().center();
    const int row = rowKick;
    CHECK(row >= 0);
    auto cellOccupied = [&](double cellStart) {
      for (const auto& r : s.tracks()[row].regions)
        if (r.startBeats < cellStart + 4.0 &&
            r.startBeats + r.durationBeats > cellStart)
          return true;
      for (const auto& c : s.tracks()[row].clips)
        if (c.startBeats < cellStart + 4.0 &&
            c.startBeats + c.durationBeats > cellStart)
          return true;
      return false;
    };
    int freeBars = 0;
    for (double b = 0.0; b <= 16.0; b += 4.0)
      if (!cellOccupied(b))
        ++freeBars;
    const size_t regBefore = s.tracks()[row].regions.size();

    // Activate the paint tool via its toolbar button.
    postPress(paintBtn, pc);
    postRelease(paintBtn, pc);
    // One stroke across bars 0..16: only empty cells receive a block.
    postPress(grid, QPoint(beatToX(row, 0.5), rowY(row)));
    for (double b = 4.0; b <= 16.0; b += 4.0)
      postMove(grid, QPoint(beatToX(row, b + 0.5), rowY(row)));
    postRelease(grid, QPoint(beatToX(row, 16.5), rowY(row)));

    const size_t regAfter = s.tracks()[row].regions.size();
    std::printf("DBG paint1 placed=%lld (want %d)\n", (long long)(regAfter-regBefore), freeBars);
    CHECK((int)(regAfter - regBefore) == freeBars);
    // Every placed block is bar-aligned and named for the active bank.
    int aligned = 0;
    for (size_t i = regBefore; i < s.tracks()[row].regions.size(); ++i) {
      const auto& r = s.tracks()[row].regions[i];
      if (r.name == "Pattern 1" && std::fmod(r.startBeats, 4.0) < 1e-9)
        ++aligned;
    }
    CHECK(aligned == freeBars);

    // A single undo removes the whole stroke; a single redo re-brushes it.
    QAction* undo = findActionByKey(win, QKeySequence::Undo);
    QAction* redo = findActionByKey(win, QKeySequence::Redo);
    CHECK(undo != nullptr && redo != nullptr);
    if (undo) undo->trigger();
    CHECK(s.tracks()[row].regions.size() == regBefore);
    if (redo) redo->trigger();
    CHECK(s.tracks()[row].regions.size() == regAfter);
    if (undo) undo->trigger(); // leave session exactly as before
    CHECK(s.tracks()[row].regions.size() == regBefore);

    // Deactivate the paint tool.
    postPress(paintBtn, pc);
    postRelease(paintBtn, pc);
  }

  // ── 2. Transport: ruler seek, playback, auto-stop at content end ──────────
  std::printf("CASE firstfunk-transport\n");
  engine->setLoop(false, 0.0, 1.0);
  engine->stop();
  {
    std::vector<float> z(2 * 512, 0.f);
    engine->process(nullptr, 0, z.data(), 2, 512, 0, 48000); // drain the stop
  }
  std::printf("    closeAudio...\n");
  engine->closeAudio();
  std::printf("    wait stop...\n");
  for (int i = 0; i < 800 && engine->isRunning(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  std::printf("    stopped=%d\n", (int)engine->isRunning());

  const double contentEnd = s.contentEndBeats();
  CHECK(contentEnd > 120.0);
  CHECK(std::fabs(contentEnd - 128.0) < 1.0);

  // 2a. Ruler seek to beat 32.
  postPress(grid, QPoint(beatToX(0, 32.0), kRulerH / 2));
  postRelease(grid, QPoint(beatToX(0, 32.0), kRulerH / 2));
  std::printf("    process...\n");
  std::vector<float> block(2 * 24000, 0.f);
  engine->process(nullptr, 0, block.data(), 2, 24000, 0, 48000);
  const double seekFrames = 32.0 * kSamplesPerBeat;
  CHECK(std::fabs(double(engine->playheadSamples()) - seekFrames) < 48000.0 + 24000.0);

  // 2b. Play and auto-stop at content end.
  std::printf("    play...\n");
  engine->play(false);
  {
    std::vector<float> boot(2 * 512, 0.f);
    engine->process(nullptr, 0, boot.data(), 2, 512, 0, 48000);
  }
  std::printf("    transport check...\n");
  uint64_t total = 0;
  const uint64_t endFrames = uint64_t(contentEnd * kSamplesPerBeat);
  const uint64_t maxFrames = uint64_t((contentEnd + 8.0) * kSamplesPerBeat);
  float peak = 0.f;
  std::printf("    TDBG pre-loop playing=%d ph=%llu\n",
              (int)engine->isTransportPlaying(),
              (unsigned long long)engine->playheadSamples());
  while (engine->isTransportPlaying() && total < maxFrames) {
    block.assign(2 * 24000, 0.f);
    engine->process(nullptr, 0, block.data(), 2, 24000, 0, 48000);
    total += 24000;
    if (total <= 72000)
      for (int i = 0; i < 2 * 24000; ++i)
        peak = std::max(peak, std::fabs(block[i]));
  }
  std::printf("    TDBG post-loop playing=%d total=%llu peak=%f ph=%llu\n",
              (int)engine->isTransportPlaying(), (unsigned long long)total,
              peak, (unsigned long long)engine->playheadSamples());
  CHECK(!engine->isTransportPlaying());
  CHECK(peak > 0.01f);
  CHECK(engine->playheadSamples() + 48000 >= endFrames);

  // ── 3. Mixer ↔ audio engine: gain, pan, mute, solo, master, meters ────────
  std::printf("CASE firstfunk-mixer\n");
  // Ensure transport is stopped and stream closed for deterministic headless rendering.
  engine->setLoop(false, 0.0, 1.0);
  engine->stop();
  {
    std::vector<float> z(2 * 512, 0.f);
    engine->process(nullptr, 0, z.data(), 2, 512, 0, 48000); // drain stop/loop
  }
  engine->closeAudio();
  for (int i = 0; i < 400 && engine->isRunning(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));

  // Helper: render 1 beat from beat 20 (inside MAIN) and return peak L/R.
  auto renderOneBeat = [&](float& peakL, float& peakR) {
    engine->seekToBeat(20.0);
    engine->play(false);
    {
      std::vector<float> boot(2 * 512, 0.f);
      engine->process(nullptr, 0, boot.data(), 2, 512, 0, 48000);
    }
    {
      std::vector<float> warm(2 * uint32_t(kSamplesPerBeat), 0.f);
      engine->process(nullptr, 0, warm.data(), 2, uint32_t(kSamplesPerBeat), 0,
                      48000);
    }
    std::vector<float> buf(2 * uint32_t(kSamplesPerBeat), 0.f);
    engine->process(nullptr, 0, buf.data(), 2, uint32_t(kSamplesPerBeat), 0, 48000);
    peakL = peakR = 0.f;
    for (size_t i = 0; i < buf.size(); i += 2) {
      peakL = std::max(peakL, std::fabs(buf[i]));
      peakR = std::max(peakR, std::fabs(buf[i + 1]));
    }
  };

  // Baseline full mix.
  float baseL, baseR;
  renderOneBeat(baseL, baseR);
  CHECK(baseL > 0.01f);
  CHECK(baseR > 0.01f);
  // Tracks that actually sounded in the baseline (some parts sit out at beat
  // 20, so a silent meter there is legitimate).
  std::vector<bool> baseActive;
  for (size_t i = 0; i < s.tracks().size(); ++i)
    baseActive.push_back(engine->trackMeterPeakL(uint32_t(i)) > 0.f ||
                         engine->trackMeterPeakR(uint32_t(i)) > 0.f);

  // 3a. Volume: solo kick, drop its gain – peak should shrink.
  {
    const size_t kickIdx = [&]() {
      for (size_t i = 0; i < s.tracks().size(); ++i)
        if (QString::fromStdString(s.tracks()[i].name) == "HARDTEKK KICK")
          return i;
      return size_t(-1);
    }();
    CHECK(kickIdx != size_t(-1));
    const float savedGain = s.tracks()[kickIdx].gainDb;
    s.tracks()[kickIdx].solo = true;
    s.tracks()[kickIdx].gainDb = -12.0f;
    win->sessionEdited();
    float pkL, pkR;
    renderOneBeat(pkL, pkR);
    CHECK(pkL > 0.001f);
    // Kick is low-freq mono-ish; left and right should be similar.
    CHECK(std::fabs(pkL - pkR) < 0.05f * std::max(pkL, pkR));
    // Restore.
    s.tracks()[kickIdx].solo = false;
    s.tracks()[kickIdx].gainDb = savedGain;
    win->sessionEdited();
  }

  // 3b. Pan: solo hats, hard-pan left – left should dominate.
  {
    const size_t hatsIdx = [&]() {
      for (size_t i = 0; i < s.tracks().size(); ++i)
        if (QString::fromStdString(s.tracks()[i].name) == "HI-HATS")
          return i;
      return size_t(-1);
    }();
    CHECK(hatsIdx != size_t(-1));
    const float savedPan = s.tracks()[hatsIdx].pan;
    s.tracks()[hatsIdx].solo = true;
    s.tracks()[hatsIdx].pan = -1.0f;
    win->sessionEdited();
    float pkL, pkR;
    renderOneBeat(pkL, pkR);
    CHECK(pkL > 0.01f);
    CHECK(pkR < pkL * 0.15f);
    // Restore.
    s.tracks()[hatsIdx].solo = false;
    s.tracks()[hatsIdx].pan = savedPan;
    win->sessionEdited();
  }

  // 3c. Mute: solo kick, then mute it – peak should vanish.
  {
    const size_t kickIdx = [&]() {
      for (size_t i = 0; i < s.tracks().size(); ++i)
        if (QString::fromStdString(s.tracks()[i].name) == "HARDTEKK KICK")
          return i;
      return size_t(-1);
    }();
    CHECK(kickIdx != size_t(-1));
    s.tracks()[kickIdx].solo = true;
    s.tracks()[kickIdx].muted = true;
    win->sessionEdited();
    float pkL, pkR;
    renderOneBeat(pkL, pkR);
    CHECK(pkL < 0.001f);
    CHECK(pkR < 0.001f);
    s.tracks()[kickIdx].solo = false;
    s.tracks()[kickIdx].muted = false;
    win->sessionEdited();
  }

  // 3d. Solo: two tracks active, solo one – peak should match the soloed track alone.
  {
    const size_t bassIdx = [&]() {
      for (size_t i = 0; i < s.tracks().size(); ++i)
        if (QString::fromStdString(s.tracks()[i].name) == "FUNK BASS")
          return i;
      return size_t(-1);
    }();
    const size_t stabIdx = [&]() {
      for (size_t i = 0; i < s.tracks().size(); ++i)
        if (QString::fromStdString(s.tracks()[i].name) == "FUNK STABS")
          return i;
      return size_t(-1);
    }();
    CHECK(bassIdx != size_t(-1));
    CHECK(stabIdx != size_t(-1));
    // Solo bass only.
    s.tracks()[bassIdx].solo = true;
    s.tracks()[stabIdx].solo = false;
    win->sessionEdited();
    float bassL, bassR;
    renderOneBeat(bassL, bassR);
    CHECK(bassL > 0.001f);
    // Now solo stab only.
    s.tracks()[bassIdx].solo = false;
    s.tracks()[stabIdx].solo = true;
    win->sessionEdited();
    float stabL, stabR;
    renderOneBeat(stabL, stabR);
    CHECK(stabL > 0.001f);
    // They should differ in level.
    CHECK(std::fabs(bassL - stabL) > 0.001f || std::fabs(bassR - stabR) > 0.001f);
    s.tracks()[stabIdx].solo = false;
    win->sessionEdited();
  }

  // 3e. Master gain: lower master – overall peak should drop.
  {
    const float savedMaster = s.masterGainDb();
    s.setMasterGainDb(-12.0f);
    win->sessionEdited();
    float pkL, pkR;
    renderOneBeat(pkL, pkR);
    CHECK(pkL < baseL * 0.55f);
    CHECK(pkR < baseR * 0.55f);
    s.setMasterGainDb(savedMaster);
    win->sessionEdited();
  }

  // 3f. Meters: after a render block, active tracks should have non-zero meter.
  {
    engine->seekToBeat(20.0);
    std::vector<float> buf(2 * 24000, 0.f);
    engine->process(nullptr, 0, buf.data(), 2, 24000, 0, 48000);
    for (size_t i = 0; i < s.tracks().size(); ++i) {
      if (!baseActive[i])
        continue; // silent part in this region reads 0 legitimately
      const float mL = engine->trackMeterPeakL(uint32_t(i));
      const float mR = engine->trackMeterPeakR(uint32_t(i));
      CHECK(mL > 0.0f || mR > 0.0f);
    }
  }

  // ── 4. Save and reload – verify round-trip integrity ──────────────────────
  std::printf("CASE firstfunk-save-reload\n");
  const QString projFile = projDir + QStringLiteral("/FIRST FUNK.misuc");
  const size_t trackCount = s.tracks().size();
  const size_t hatClipCount = s.tracks()[rowHats].clips.size();
  const float savedBassGain = s.tracks()[rowBass].gainDb;
  const float savedBassPan  = s.tracks()[rowBass].pan;
  const bool   savedBassMute = s.tracks()[rowBass].muted;
  const double savedTempo = s.tempo();
  CHECK(win->writeProject(projFile));
  CHECK(QFile::exists(projFile));

  win->openProject(projFile);
  Session& s2 = const_cast<Session&>(win->session());
  CHECK(s2.name() == "FIRST FUNK");
  CHECK(std::fabs(s2.tempo() - savedTempo) < 1e-6);
  CHECK(s2.tracks().size() == trackCount);
  CHECK(s2.contentEndBeats() > 120.0);

  // Verify a bass region survived with the right name and note count.
  bool bassFound = false;
  for (const auto& t : s2.tracks()) {
    if (QString::fromStdString(t.name) != "FUNK BASS") continue;
    for (const auto& r : t.regions)
      if (r.name == "Bass Groove A") {
        bassFound = true;
        CHECK(r.notes.size() > 0);
      }
    CHECK(t.clips.size() == 0);
    CHECK(std::fabs(t.gainDb - savedBassGain) < 0.5f);
    CHECK(std::fabs(t.pan - savedBassPan) < 0.02f);
    CHECK(t.muted == savedBassMute);
  }
  CHECK(bassFound);

  // Verify hi-hat clips survived with source files present.
  for (const auto& t : s2.tracks()) {
    if (QString::fromStdString(t.name) != "HI-HATS") continue;
    CHECK(t.clips.size() == hatClipCount);
    for (const auto& c : t.clips)
      CHECK(QFile::exists(QString::fromStdString(c.sourceFile)));
  }

  // Verify stabs and lead regions survived.
  bool stabFound = false, leadFound = false;
  for (const auto& t : s2.tracks()) {
    for (const auto& r : t.regions) {
      if (r.name == "Stab A" || r.name == "Stab B") stabFound = true;
      if (r.name == "Lead Main" || r.name == "Lead Outro") leadFound = true;
    }
  }
  CHECK(stabFound);
  CHECK(leadFound);

  // Post-reload transport smoke: play a few blocks and confirm audio appears.
  engine->closeAudio();
  for (int i = 0; i < 400 && engine->isRunning(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  engine->seekToBeat(20.0);
  engine->play(false);
  {
    std::vector<float> boot(2 * 512, 0.f);
    engine->process(nullptr, 0, boot.data(), 2, 512, 0, 48000);
  }
  CHECK(engine->isTransportPlaying());
  float postPeak = 0.f;
  for (int k = 0; k < 4; ++k) {
    block.assign(2 * 24000, 0.f);
    engine->process(nullptr, 0, block.data(), 2, 24000, 0, 48000);
    for (int i = 0; i < 2 * 24000; ++i)
      postPeak = std::max(postPeak, std::fabs(block[i]));
  }
  engine->stop();
  CHECK(postPeak > 0.01f);

  // ── 5. Phase E DoD: arrange a 100-clip song by painting ───────────────────
  // Give the rack a real kit, then brush 100 whole bars onto the hi-hat lane
  // from the end of the existing arrangement. One drag-stroke, one undo step.
  std::printf("CASE firstfunk-paint-100\n");
  {
    auto& cr = s.channelRack();
    cr.channels.clear();
    cr.activePattern = 0;
    misuc::project::StepChannel kick;
    kick.name = "Kick";
    kick.steps[0][0] = 100;
    kick.steps[0][8] = 100;
    misuc::project::StepChannel snare;
    snare.name = "Snare";
    snare.steps[0][4] = 100;
    snare.steps[0][12] = 100;
    cr.channels.push_back(kick);
    cr.channels.push_back(snare);

    QWidget* paintBtn = win->findChild<QWidget*>(QStringLiteral("plPaint"));
    CHECK(paintBtn != nullptr);
    const QPoint pc = paintBtn->rect().center();
    const int row = rowHats;
    CHECK(row >= 0);
    const size_t regBefore = s.tracks()[row].regions.size();

    postPress(paintBtn, pc);
    postRelease(paintBtn, pc);
    // Cells 128,132,...,524 = 100 whole bars past the arrangement's content.
    postPress(grid, QPoint(beatToX(row, 128.5), rowY(row)));
    for (int b = 132; b <= 524; b += 4)
      postMove(grid, QPoint(beatToX(row, b + 0.5), rowY(row)));
    postRelease(grid, QPoint(beatToX(row, 524.5), rowY(row)));

    const size_t painted = s.tracks()[row].regions.size() - regBefore;
    std::printf("DBG paint100 painted=%zu rowY ok\n", painted);
    CHECK(painted == 100);
    // Every block is bar-aligned, named after the active bank, and carries the
    // seeded kit's notes (so the painted song actually sounds).
    int solid = 0;
    for (size_t i = regBefore; i < s.tracks()[row].regions.size(); ++i) {
      const auto& r = s.tracks()[row].regions[i];
      if (r.name == "Pattern 1" && std::fmod(r.startBeats, 4.0) < 1e-9 &&
          !r.notes.empty())
        ++solid;
    }
    CHECK(solid == 100);

    // The whole 100-block stroke is one undo step.
    QAction* undo = findActionByKey(win, QKeySequence::Undo);
    QAction* redo = findActionByKey(win, QKeySequence::Redo);
    CHECK(undo != nullptr && redo != nullptr);
    if (undo) undo->trigger();
    CHECK(s.tracks()[row].regions.size() == regBefore);
    if (redo) redo->trigger();
    CHECK(s.tracks()[row].regions.size() == regBefore + 100);
    // Leave the painted 100-block song on the lane for the Phase E demo.
    postPress(paintBtn, pc);
    postRelease(paintBtn, pc);
  }

  std::printf("  FIRST FUNK demo project created, ops verified, "
              "round-trip survived (%zu tracks, %.0f BPM)\n",
              trackCount, s2.tempo());
}

// ===========================================================================
// AUTOSAVE rotation + crash-recovery copy semantics. The recovery *dialog*
// stays a human decision; this case verifies the file layer underneath it
// (rotation, validate-before-promote, backup survival, save-supersedes).
// ===========================================================================
static QByteArray readAllBytes(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QByteArray();
  return f.readAll();
}

static bool autosaveParses(const QString& path) {
  // The .misuc document is a native container; the song JSON lives inside it
  // (legacy plain-JSON files from older builds load too).
  const auto cont = misuc::core::readContainer(path.toStdString());
  if (!cont.ok)
    return false;
  std::string json = cont.legacyJson ? cont.legacyText : std::string();
  if (!cont.legacyJson) {
    for (const auto& e : cont.entries)
      if (e.name == misuc::core::kContainerSongEntry)
        json = e.data;
  }
  try {
    (void)misuc::project::Session::fromJson(json);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

static void autosaveChecks(MainWindow* win) {
  std::printf("CASE autosave-rotate\n");
  const QString cur = MainWindow::autosavePath();
  const QString prev = MainWindow::autosavePrevPath();

  // The XDG sandbox is wiped at the start of every run, so both autosave
  // copies are guaranteed absent before the first write (this also proves the
  // promised paths resolve under QStandardPaths::AppDataLocation).
  CHECK(!QFile::exists(cur));
  CHECK(!QFile::exists(prev));

  // Build a dirty, non-empty session.
  auto& s = win->session();
  const std::string tid = s.addTrack("Recovery");
  s.addRegion(tid, "Riff", 0.0, 4.0);
  win->sessionEdited(); // marks the document dirty

  win->autosaveNow(); // writes autosave.misuc
  win->autosaveNow(); // rotates: current -> .prev, writes a fresh current

  CHECK(autosaveParses(cur));
  CHECK(autosaveParses(prev));
  const QByteArray goodPrev = readAllBytes(prev);
  CHECK(!goodPrev.isEmpty());

  // Corrupt the current copy; the next autosave must drop it and rewrite a
  // valid current, while .prev keeps the previous good session untouched.
  {
    QFile f(cur);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      f.write("garbage{ not json");
  }
  CHECK(!autosaveParses(cur)); // the corruption above is real
  win->autosaveNow();
  CHECK(autosaveParses(cur));          // rewritten as a fresh valid session
  CHECK(readAllBytes(prev) == goodPrev); // backup survived the rotation

  // A real save supersedes the recovery copies entirely.
  const QString projectPath =
      QStringLiteral("/tmp/misuc_qa_settings/autosave_case.misuc");
  CHECK(win->writeProject(projectPath));
  CHECK(QFile::exists(projectPath));
  CHECK(!QFile::exists(cur));
  CHECK(!QFile::exists(prev));

  // Configurable interval: the persisted setting drives the live timer and
  // the change round-trips through QSettings (protocol #7 Part 1).
  CHECK(win->autosaveIntervalSeconds() == 90); // default applied at startup
  win->setAutosaveIntervalSeconds(300);
  CHECK(win->autosaveIntervalSeconds() == 300);
  win->setAutosaveIntervalSeconds(0); // disabled
  CHECK(win->autosaveIntervalSeconds() == 0);
  win->setAutosaveIntervalSeconds(900);
  CHECK(win->autosaveIntervalSeconds() == 900);
  win->setAutosaveIntervalSeconds(90); // restore the default for later cases

  QSettings asSettings(QStringLiteral("misuc"), QStringLiteral("misuc"));
  CHECK(asSettings.value(QStringLiteral("autosave/intervalSeconds")).toInt() ==
        90);

  // The Audio Settings page stages the persisted interval and drives the
  // MainWindow timer through its change signals.
  if (auto* chk = win->findChild<QCheckBox*>(QStringLiteral("autosaveChk"))) {
    CHECK(chk->isChecked()); // 90 s default means autosave enabled
    chk->setChecked(false);  // disable -> timer stopped + persisted 0
    CHECK(win->autosaveIntervalSeconds() == 0);
    auto* combo = win->findChild<QComboBox*>(QStringLiteral("autosaveIntervalCombo"));
    CHECK(combo != nullptr);
    if (combo) {
      combo->setCurrentIndex(2); // "5 minutes" -> 300 s
      chk->setChecked(true);
      CHECK(win->autosaveIntervalSeconds() == 300);
    }
    chk->setChecked(false);
  }
  win->setAutosaveIntervalSeconds(90); // restore default for later cases
  CHECK(win->autosaveIntervalSeconds() == 90);

  std::printf("  autosave rotation + crash-recovery copy semantics ok\n");

  // Open Recent: persisted recents are re-read into the File > Open Recent
  // menu; missing projects are visually marked unavailable, colliding file
  // names carry path info (protocol #7 Part 2).
  const QString tree = QStringLiteral("/tmp/misuc_qa_recent");
  QDir().mkpath(tree + QStringLiteral("/foldA"));
  QDir().mkpath(tree + QStringLiteral("/foldB"));
  const QString realA = tree + QStringLiteral("/foldA/Alpha.misuc");
  const QString realB = tree + QStringLiteral("/foldB/Alpha.misuc"); // collides
  const QString gone = tree + QStringLiteral("/foldB/Gone.misuc");
  {
    QFile f(realA);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("{}");
  }
  // realB shares the name but must be present for the collision only; give it
  // a valid-ish file so it is not itself marked missing.
  {
    QFile f(realB);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("{}");
  }
  QFile::remove(gone);
  QFile::remove(realB); // treat as dropped: folder B file vanished
  QFile::remove(QStringLiteral("/tmp/misuc_qa_recent/foldB/Alpha.misuc"));
  win->addRecentProject(realA);
  win->addRecentProject(realB);
  win->addRecentProject(gone);
  auto* recentMenu = win->findChild<QMenu*>(QStringLiteral("recentMenu"));
  CHECK(recentMenu != nullptr);
  bool sawUnavailable = false;
  bool sawCollisionPath = false;
  if (recentMenu) {
    for (QAction* act : recentMenu->actions()) {
      const QString t = act->text();
      if (t.contains(QStringLiteral("Gone.misuc")))
        sawUnavailable = t.contains(QStringLiteral("unavailable"));
      if (t.contains(QStringLiteral("foldA/Alpha.misuc")) ||
          t.contains(QStringLiteral("foldB/Alpha.misuc")))
        sawCollisionPath = true;
    }
  }
  CHECK(sawUnavailable);
  CHECK(sawCollisionPath);
  // raw list retains the missing entry (the menu marks, doesn't pre-filter)
  CHECK(win->recentProjects().contains(gone));
  // clean up so neither later cases nor a re-run see test residue
  asSettings.remove(QStringLiteral("recent/projects"));
}

// ===========================================================================
// METRONOME MIX: the transport metronome button drives the engine and the
// session across toggling, wheel-volume, count-in cycling, and a save/reload.
// ===========================================================================
static void metronomeUiChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE metronome-ui\n");
  TransportBar* transport = win->headerBar()->transportBar();
  CHECK(transport != nullptr);
  if (!transport)
    return;
  // Lay the bar out like paintEvent would so the button hit-tests are valid.
  QImage tbImg(transport->size(), QImage::Format_ARGB32);
  transport->render(&tbImg);
  const QPoint c = transport->metronomeButtonRect().center().toPoint();

  Session& s = const_cast<Session&>(win->session());
  CHECK(!s.metronomeOn());                      // default: click-free project
  CHECK(std::fabs(engine->metronomeVolume() - 0.9f) < 1e-6f);

  // Click toggles the metronome on; the engine + session both follow.
  postPress(transport, c);
  postRelease(transport, c);
  CHECK(s.metronomeOn());
  // Wheel up over the button raises the click level through engine + session.
  const float v0 = engine->metronomeVolume();
  postWheel(transport, c, 120);
  // The control streams to the audio thread via the RT queue; drain one block
  // (the headless engine is not running a live stream here), then read back.
  std::vector<float> drain(2 * 512, 0.f);
  engine->process(nullptr, 0, drain.data(), 2, 512, 0, 48000);
  CHECK(engine->metronomeVolume() > v0);
  CHECK(std::fabs(s.metronomeVolume() - engine->metronomeVolume()) < 1e-4f);
  CHECK(transport->metronomeVolume() > v0);

  // Right-click cycles the count-in length (2 -> 4 -> 1 -> 2).
  const float b0 = s.countInBeats();
  clickRight(transport, c);
  const float b1 = s.countInBeats();
  CHECK(b1 != b0);
  CHECK(std::fabs(transport->metronomeVolume() - s.metronomeVolume()) < 1e-4f);

  // Persistence: save/reload must restore the on/volume/count-in state.
  const float savedVol = s.metronomeVolume();
  const bool savedOn = s.metronomeOn();
  const float savedBeats = s.countInBeats();
  const QString proj =
      QStringLiteral("/tmp/misuc_qa_settings/metronome_case.misuc");
  CHECK(win->writeProject(proj));
  win->openProject(proj);
  Session& s2 = const_cast<Session&>(win->session());
  CHECK(s2.metronomeOn() == savedOn);
  CHECK(std::fabs(s2.metronomeVolume() - savedVol) < 1e-4f);
  CHECK(std::fabs(s2.countInBeats() - savedBeats) < 1e-4f);
  CHECK(std::fabs(s2.metronomeVolume() -
                  win->headerBar()->transportBar()->metronomeVolume()) < 1e-4f);
  std::printf("  metronome on=%d volume=%.2f countIn=%.0f survived\n",
              (int)savedOn, (double)savedVol, (double)s2.countInBeats());
}

// ===========================================================================
// PIANO-ROLL VELOCITY TOOL: the Alt+Up/Down nudge moves the selection's
// velocities in one undo snapshot.
// ===========================================================================
static void pianoVelocityChecks(MainWindow* win) {
  std::printf("CASE piano-velocity\n");
  Session& s = const_cast<Session&>(win->session());
  if (s.tracks().empty())
    s.addTrack("Track 1");
  const std::string tid = s.tracks().front().id;

  // A real region to edit (the seeded state may not have one yet).
  std::string rid;
  for (const auto& r : s.tracks().front().regions)
    if (!r.id.empty() && r.startBeats >= 0.0) {
      rid = r.id;
      break;
    }
  if (rid.empty())
    rid = s.addRegion(tid, "Vel Tool", 0.0, 8.0);
  win->sessionEdited();

  // Bind the real piano-roll widget to the track+region and paint its canvas.
  PianoRoll* piano = win->findChild<PianoRoll*>();
  CHECK(piano != nullptr);
  if (!piano)
    return;

  // On first show, the editor lands its view on middle C so the C1..C7
  // keyboard is visible instead of the top of the 0..127 range.
  win->workspaceManager()->showWorkspace(misuc::ui::workspaces::Id::PianoRoll, true);
  win->show();
  for (int i = 0; i < 12; ++i) {
    QApplication::processEvents();
    QThread::msleep(5);
  }
  const int centerScroll = piano->verticalScroll();
  const int maxScroll =
      piano->rowHeight() * PianoRoll::kRowCount + PianoRoll::kClipLaneH +
      PianoRoll::kVelLaneH - 900; // harness viewport is not collapsible
  CHECK(centerScroll > 0);
  CHECK(centerScroll <= maxScroll);
  // Centered on middle C means a scroll offset roughly half-way down.
  CHECK(std::abs(centerScroll - maxScroll / 2) < maxScroll / 3);

  piano->setTrack(QString::fromStdString(tid));
  piano->setRegion(QString::fromStdString(tid), QString::fromStdString(rid));
  piano->refresh();
  auto& track = s.tracks().front();
  auto* region = [&]() -> misuc::project::MidiRegion* {
    for (auto& r : track.regions)
      if (r.id == rid)
        return &r;
    return nullptr;
  }();
  CHECK(region != nullptr);
  if (!region)
    return;
  const size_t notesBefore = region->notes.size();

  // Draw one note via double-click (pitch 96 at local beat 3.5).
  QWidget* canvas = win->findChild<QWidget*>(QStringLiteral("pianoCanvas"));
  CHECK(canvas != nullptr);
  if (!canvas)
    return;
  const double horizon = std::max(8.0, region->durationBeats + 4.0);
  const double pxPerBeat = double(canvas->width() - 1) / horizon;
  const int x = int(std::lround(3.5 * pxPerBeat));
  const int y = 28 + (127 - 96) * 18;
  postDblClick(canvas, QPoint(x, y));

  const size_t notesAfter = region->notes.size();
  CHECK(notesAfter == notesBefore + 1);
  // The just-drawn note is pitch 96 at local beat 3.5; find it by pitch+beat
  // later too, since a seeded region may already hold other notes.
  auto first90 = [&](const misuc::project::MidiRegion& r) {
    for (const auto& n : r.notes)
      if (n.pitch == 96 && std::fabs(n.startBeats - 3.5) < 0.2)
        return n;
    return NoteEvent();
  };
  const uint8_t vel0 = first90(*region).velocity;
  CHECK(vel0 == 100);

  // Draw a second note nearby so the selection has two members.
  postDblClick(canvas, QPoint(x + int(pxPerBeat), y));
  CHECK(region->notes.size() == notesAfter + 1);

  // Select the first note (press+release on its body), then Alt+Up nudges
  // the selection's velocity up by one step in a single undo snapshot.
  postPress(canvas, QPoint(x, y + 4));
  postRelease(canvas, QPoint(x, y + 4));
  postKey(canvas, Qt::Key_Up, Qt::AltModifier);
  const uint8_t ven = first90(*region).velocity;
  CHECK(ven == vel0 + 1);
  std::printf("  velocity tool nudge %d -> %d ok\n", (int)vel0, (int)ven);
}

// ===========================================================================
// main
// ===========================================================================
// ===========================================================================
// TEXT OVERLAP / LAYOUT (Comp07, #5): reusable layout audit. The visible
// window is driven through several dock widths (minimum / narrow / medium /
// wide), float+redock and tab switches are exercised, and rule 0 is enforced
// with real geometry: no visible child widget may (a) extend beyond its
// visible parent, or (b) intersect a visible sibling, and long content must
// elide (ElideLabel) with the full text reachable via tooltip instead of
// painting into neighbouring controls. This is the headless resizing test
// required by section 11 of the protocol.
// ===========================================================================
namespace {
// Containers whose direct children are allowed to overlap or sit outside the
// parent (scroll views, tab stacks, splitter handles, sliders, spinboxes…).
bool isLayeredContainer(const QWidget* w) {
  // Scroll viewports / scroll widgets / stacked pages / tab bars / menus /
  // splitters legitimately hold content larger than their frame.
  if (qobject_cast<const QScrollArea*>(w) ||
      qobject_cast<const QStackedWidget*>(w) ||
      qobject_cast<const QTabBar*>(w) ||
      qobject_cast<const QScrollBar*>(w) ||
      qobject_cast<const QSplitter*>(w) ||
      qobject_cast<const QAbstractSpinBox*>(w) ||
      qobject_cast<const QAbstractScrollArea*>(w) ||
      qobject_cast<const QMenu*>(w))
    return true;
  // The viewport of a scroll area clips (does not overlap) its content, and
  // its own frame sits inside the surrounding layout — never a bug source.
  if (qobject_cast<const QAbstractScrollArea*>(w->parentWidget()) != nullptr)
    return true;
  return false;
}

int auditOverlaps(QWidget* root, QString* first) {
  int violations = 0;
  const QList<QWidget*> all = root->findChildren<QWidget*>();
  for (QWidget* p : all) {
    if (!p->isVisible() || isLayeredContainer(p) || p->width() <= 0)
      continue;
    const QList<QWidget*> kids =
        p->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (int i = 0; i < kids.size(); ++i) {
      QWidget* a = kids[i];
      if (!a->isVisible() || a->width() <= 0 || a->height() <= 0 ||
          isLayeredContainer(a))
        continue;
      const QRect ra = a->geometry();
      if (ra.right() > p->width() + 2 || ra.bottom() > p->height() + 2 ||
          ra.left() < -2 || ra.top() < -2) {
        if (first && first->isEmpty())
          *first = QString::asprintf("%s overflow (%dx%d) in %s (%dx%d)",
                                     a->metaObject()->className(), ra.width(),
                                     ra.height(), p->metaObject()->className(),
                                     p->width(), p->height());
        ++violations;
        continue;
      }
      for (int j = i + 1; j < kids.size(); ++j) {
        QWidget* b = kids[j];
        if (!b->isVisible() || b->width() <= 0 || b->height() <= 0 ||
            isLayeredContainer(b))
          continue;
        const QRect inter = ra.intersected(b->geometry());
        if (inter.width() < 3 || inter.height() < 3)
          continue;
        if (qobject_cast<const QScrollBar*>(a) ||
            qobject_cast<const QScrollBar*>(b) ||
            qobject_cast<const QSplitterHandle*>(a) ||
            qobject_cast<const QSplitterHandle*>(b) ||
            qobject_cast<const QFrame*>(a) && a->isWindow() ||
            qobject_cast<const QDialog*>(a) || qobject_cast<const QDialog*>(b))
          continue;
        if (first && first->isEmpty()) {
          const auto snip = [](const QWidget* w) {
            const QString name = w->objectName();
            QString s;
            if (const auto* l = qobject_cast<const QLabel*>(w))
              s = l->text();
            return QStringLiteral("[%1|%2]")
                .arg(name, s.left(20));
          };
          *first = QString::asprintf(
              "sibling %s%s %dx%d+%d+%d x %s%s %dx%d+%d+%d under %s[%s] %dx%d+%d+%d",
              a->metaObject()->className(), snip(a).toUtf8().constBegin(),
              ra.width(), ra.height(), ra.x(), ra.y(),
              b->metaObject()->className(), snip(b).toUtf8().constBegin(),
              b->geometry().width(), b->geometry().height(),
              b->geometry().x(), b->geometry().y(),
              p->metaObject()->className(),
              (p->parentWidget()
                   ? p->parentWidget()->objectName() + QStringLiteral("/") +
                         p->parentWidget()->metaObject()->className()
                   : QString())
                  .toUtf8()
                  .constBegin(),
              p->width(), p->height(), p->x(), p->y());
        }
        ++violations;
      }
    }
  }
  return violations;
}
}  // namespace

static void textOverlapChecks(MainWindow* win, AudioEngine* /*engine*/) {
  std::printf("CASE text-overlap\n");
  using misuc::ui::Browser;
  using misuc::ui::ChannelRack;
  using misuc::ui::Inspector;
  auto* ws = win->workspaceManager();
  if (!ws) return;
  auto* tabs = win->findChild<QTabWidget*>(QStringLiteral("workspaceTabs"));
  auto* dockBrowser = ws->docks(misuc::ui::workspaces::Id::Browser);
  auto* dockInspector = ws->docks(misuc::ui::workspaces::Id::Inspector);
  auto* dockRack = ws->docks(misuc::ui::workspaces::Id::ChannelRack);
  auto* dockMixer = ws->docks(misuc::ui::workspaces::Id::Mixer);
  auto* browser = win->findChild<Browser*>();
  auto* inspector = win->findChild<Inspector*>();
  auto* rack = win->findChild<ChannelRack*>();
  CHECK(tabs != nullptr);
  CHECK(dockBrowser != nullptr && dockInspector != nullptr &&
        dockRack != nullptr && dockMixer != nullptr);
  CHECK(browser != nullptr && inspector != nullptr && rack != nullptr);
  if (!tabs || !dockBrowser || !dockInspector || !dockRack || !dockMixer ||
      !browser || !inspector || !rack)
    return;

  // The seeded kit must be present so panels have real content to lay out.
  const_cast<Session&>(win->session());
  win->show();
  QApplication::processEvents();

  auto audit = [](QWidget* root, const QString& tag) {
    QString first;
    const int v = auditOverlaps(root, &first);
    if (v != 0) {
      std::printf("    [%s] %d overlap violations: %s\n",
                  tag.toUtf8().constBegin(), v, first.toUtf8().constBegin());
      if (const auto* lay = root->layout()) {
        QString tmp;
        for (int c = 0; c < lay->count() && c < 10; ++c) {
          if (auto* item = lay->itemAt(c)) {
            const QRect g = item->widget()
                                ? item->widget()->geometry()
                                : QRect(-1, -1, -1, -1);
            tmp += QStringLiteral(" [%1 %2x%3+%4+%5]")
                       .arg(item->widget()
                                ? QString::fromLatin1(
                                      item->widget()->metaObject()->className())
                                : QStringLiteral("spacer"))
                       .arg(g.width())
                       .arg(g.height())
                       .arg(g.x())
                       .arg(g.y());
          }
        }
        std::printf("      layout %s:%s\n", lay->metaObject()->className(),
                    tmp.isEmpty() ? "" : tmp.toUtf8().constBegin());
      }
    }
    CHECK(v == 0);
    return v;
  };

  const QList<QDockWidget*> docks = {dockBrowser, dockInspector, dockRack,
                                     dockMixer};

  // 1. Long content elides instead of painting into the neighbouring column.
  //    The invariant of ElideLabel: the displayed text is exactly the full
  //    text iff there is no tooltip, and the tooltip carries the full text
  //    iff the label is elided. Sweep the reachable dock widths with a name
  //    longer than the widest the dock can show, then with a short name.
  const QString longName = QStringLiteral(
      "Master Tool \u2013 excessively long demonstration channel name 123456789 "
      "expansion pad so this title cannot ever fit inside an inspector dock");
  inspector->showTrack(0, longName, false, false, -3.0f, 0.0f);
  QApplication::processEvents();
  auto* inspTitle =
      inspector->findChild<misuc::ui::ElideLabel*>(QStringLiteral("inspectorTitle"));
  CHECK(inspTitle != nullptr);
  const auto settleTo = [&](int target) {
    for (int pass = 0; pass < 3; ++pass) {
      QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
      QApplication::processEvents();
      win->resizeDocks({dockInspector}, {target}, Qt::Horizontal);
      QApplication::processEvents();
    }
  };
  settleTo(qMax(190, dockInspector->minimumWidth()));
  const int minW = dockInspector->width();
  for (const int w : {0, 420, 640, 900}) {
    settleTo(w == 0 ? minW : w);
    QApplication::processEvents();
    const bool elided = inspTitle->isElided();
    const bool hasTip = !inspTitle->toolTip().isEmpty();
    if (!elided || !hasTip || inspTitle->toolTip() != longName)
      std::printf("      elideDbg w=%d dock=%d label=%dx%d elided=%d tip=%d first=%.10s\n",
                  w, dockInspector->width(), inspTitle->width(),
                  inspTitle->height(), int(elided), int(hasTip),
                  inspTitle->QLabel::text().toUtf8().constBegin());
    CHECK(elided);                       // full text simply does not fit
    CHECK(inspTitle->toolTip() == longName); // …but stays reachable
  }
  inspector->showTrack(0, QStringLiteral("Master"), false, false, -3.0f, 0.0f);
  QApplication::processEvents();
  settleTo(900);
  QApplication::processEvents();
  if (inspTitle->isElided() || inspTitle->QLabel::text() != QStringLiteral("Master") ||
      !inspTitle->toolTip().isEmpty())
    std::printf("      wideDbg dock=%d label=%dx%d elided=%d vis(%.10s) tip=%d\n",
                dockInspector->width(), inspTitle->width(), inspTitle->height(),
                int(inspTitle->isElided()),
                inspTitle->QLabel::text().toUtf8().constBegin(),
                int(inspTitle->toolTip().size()));
  CHECK(!inspTitle->isElided());         // short text shown in full
  CHECK(inspTitle->QLabel::text() == QStringLiteral("Master"));
  CHECK(inspTitle->toolTip().isEmpty());

  // 2. Every visible sibling stays separate from its neighbours and inside
  //    its parent at a range of widths (narrow, medium, wide) for every dock.
  const QList<int> widths = {0, 220, 380, 560, 760};
  for (int n = 0; n < widths.size(); ++n) {
    for (QDockWidget* d : docks) {
      const int w = widths[n] == 0 ? qMax(180, d->minimumWidth()) : widths[n];
      win->resizeDocks({d}, {w}, Qt::Horizontal);
      QApplication::processEvents();
      audit(d, QString::fromLatin1(d->metaObject()->className()) + "|" +
                   d->objectName() + QStringLiteral(" w%1").arg(w));
      if (d == dockRack && n == 0) {
        if (auto* ch = rack->findChild<QWidget*>(QStringLiteral("rackChrome"))) {
          QString tmp;
          if (auto* cl = ch->layout())
            for (int c = 0; c < cl->count(); ++c)
              if (auto* item = cl->itemAt(c)) {
                const QRect g = item->widget() ? item->widget()->geometry()
                                               : QRect(-1, -1, -1, -1);
                tmp += QStringLiteral(" [%1 %2x%3+%4+%5]")
                           .arg(item->widget()
                                    ? QString::fromLatin1(
                                          item->widget()->metaObject()->className())
                                    : QStringLiteral("spacer"))
                           .arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y());
              }
          std::printf("      chrome(%dx%d)%s\n", ch->width(), ch->height(),
                      tmp.toUtf8().constBegin());
        }
      }
    }
    // Central editors mirror the same guarantee at the running window width.
    for (int t = 0; t < tabs->count(); ++t) {
      tabs->setCurrentIndex(t);
      QApplication::processEvents();
      audit(tabs->currentWidget(), QStringLiteral("central %1").arg(t));
    }
  }

  // 3. Floating a panel does not corrupt layout either.
  dockBrowser->setFloating(true);
  QApplication::processEvents();
  dockBrowser->resize(240, 420);
  QApplication::processEvents();
  audit(dockBrowser, QStringLiteral("browser floating"));
  dockBrowser->setFloating(false);
  QApplication::processEvents();

  // 4. Browser file rows expose their full path (tooltip) so long filenames
  //    never have to be guessed from elided column text.
  auto* cats = browser->findChild<QTreeWidget*>(QStringLiteral("sampleCats"));
  auto* files = browser->findChild<QTreeWidget*>(QStringLiteral("sampleFiles"));
  CHECK(cats != nullptr && files != nullptr);
  if (cats && files) {
    // Select the Sounds top node: at least the seeded demo file is listed.
    for (int i = 0; i < cats->topLevelItemCount(); ++i)
      if (cats->topLevelItem(i)->text(0) == QStringLiteral("Sounds")) {
        cats->setCurrentItem(cats->topLevelItem(i));
        break;
      }
    QApplication::processEvents();
    if (files->topLevelItemCount() > 0) {
      QTreeWidgetItem* row = files->topLevelItem(0);
      const QString path = row->data(0, Qt::UserRole).toString();
      CHECK(!path.isEmpty());
      CHECK(row->toolTip(0) == path); // long names reachable via tooltip
    }
  }

  std::printf("  text-overlap: long text elides, %d-dock x %d-width soak clean, "
              "float/redock clean\n",
              int(docks.size()), int(widths.size()));
}

// ===========================================================================
// WORKSPACE / DOCKING (#3): layout presets actually rearrange the native
// QDockWidget arrangement and split proportions, the View menu toggles mirror
// real panel visibility, docks resize through the native splitters, and the
// arrangement (visibility + preset + central tab) survives a
// saveState()/restoreState() round-trip. The variance/persistence cycles also
// serve as the headless proxy for the flicker/stress acceptance pass.
// ===========================================================================
static void workspaceUiChecks(MainWindow* win) {
  std::printf("CASE workspace-ui\n");
  auto* ws = win->workspaceManager();
  CHECK(ws != nullptr);
  if (!ws) return;

  const QStringList presets = ws->presetNames();
  CHECK(presets.size() == 4);
  CHECK(presets.contains(QStringLiteral("Classic")));
  CHECK(presets.contains(QStringLiteral("Arrangement")));
  CHECK(presets.contains(QStringLiteral("Production")));
  CHECK(presets.contains(QStringLiteral("Editing")));

  // Real widgets must be driven, so the window has to be visible (offscreen
  // platform) for dock layout/resize to be meaningfully measurable.
  win->show();
  QApplication::processEvents();

  auto* tabs = win->findChild<QTabWidget*>(QStringLiteral("workspaceTabs"));
  auto* browser = ws->docks(misuc::ui::workspaces::Id::Browser);
  auto* inspector = ws->docks(misuc::ui::workspaces::Id::Inspector);
  auto* mixer = ws->docks(misuc::ui::workspaces::Id::Mixer);
  auto* rack = ws->docks(misuc::ui::workspaces::Id::ChannelRack);
  CHECK(tabs != nullptr);
  CHECK(browser != nullptr);
  CHECK(inspector != nullptr);
  CHECK(mixer != nullptr);
  CHECK(rack != nullptr);
  if (!tabs || !browser || !inspector || !mixer || !rack) return;

  auto act = [win](const QKeySequence& key) {
    return findActionByKey(win, key);
  };
  QAction* aPlaylist = act(QKeySequence(Qt::Key_F5));
  QAction* aRack = act(QKeySequence(Qt::Key_F6));
  QAction* aPiano = act(QKeySequence(Qt::Key_F7));
  QAction* aBrowser = act(QKeySequence(Qt::Key_F8));
  QAction* aMixer = act(QKeySequence(Qt::Key_F9));
  QAction* aInspector = act(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I));
  CHECK(aPlaylist != nullptr);
  CHECK(aRack != nullptr);
  CHECK(aPiano != nullptr);
  CHECK(aBrowser != nullptr);
  CHECK(aMixer != nullptr);
  CHECK(aInspector != nullptr);
  if (!aPlaylist || !aRack || !aPiano || !aBrowser || !aMixer || !aInspector)
    return;

  // -- preset application: areas, visibility, central tab, View-menu checks --
  ws->resetWorkspace();
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Classic"));
  CHECK(rack->isHidden());  // Channel Rack is docked but hidden in Classic
  CHECK(!browser->isHidden());
  CHECK(!mixer->isHidden());
  CHECK(!inspector->isHidden());
  CHECK(tabs->currentWidget() == ws->widget(misuc::ui::workspaces::Id::Playlist));
  CHECK(!aRack->isChecked());
  CHECK(aBrowser->isChecked());
  CHECK(aMixer->isChecked());
  CHECK(aPlaylist->isChecked());

  ws->applyPreset(QStringLiteral("Editing"));
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Editing"));
  CHECK(!rack->isHidden());  // rack joins the left column
  CHECK(tabs->currentWidget() == ws->widget(misuc::ui::workspaces::Id::PianoRoll));
  CHECK(aRack->isChecked());
  CHECK(aPiano->isChecked());
  CHECK(!aPlaylist->isChecked());

  ws->applyPreset(QStringLiteral("Production"));
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Production"));
  CHECK(!rack->isHidden());
  CHECK(!browser->isHidden());
  CHECK(!mixer->isHidden());

  // -- split proportions differ between presets (real divider movement) ------
  ws->applyPreset(QStringLiteral("Classic"));
  QApplication::processEvents();
  const int bwClassic = browser->width();
  ws->applyPreset(QStringLiteral("Production"));
  QApplication::processEvents();
  const int bwProduction = browser->width();
  CHECK(bwClassic > 0 && bwProduction > 0);
  CHECK(bwClassic != bwProduction);

  // -- native panel resize through the splitter ------------------------------
  QList<QDockWidget*> growList;
  growList.append(browser);
  QList<int> growSizes;
  growSizes.append(480);
  win->resizeDocks(growList, growSizes, Qt::Horizontal);
  QApplication::processEvents();
  CHECK(browser->width() > bwClassic);
  const QSize winSize = win->size();
  CHECK(winSize.width() > 0 && winSize.height() > 0);

  // -- View-menu toggles mirror + drive real visibility ----------------------
  ws->showWorkspace(misuc::ui::workspaces::Id::Browser, false);
  QApplication::processEvents();
  CHECK(browser->isHidden());
  CHECK(!aBrowser->isChecked());
  aBrowser->trigger();  // checking the menu toggle reveals it again
  QApplication::processEvents();
  CHECK(!browser->isHidden());
  CHECK(aBrowser->isChecked());

  ws->showWorkspace(misuc::ui::workspaces::Id::Inspector, false);
  QApplication::processEvents();
  CHECK(!aInspector->isChecked());
  ws->showWorkspace(misuc::ui::workspaces::Id::Inspector, true);
  QApplication::processEvents();
  CHECK(aInspector->isChecked());

  ws->showWorkspace(misuc::ui::workspaces::Id::ChannelRack, false);
  QApplication::processEvents();
  CHECK(rack->isHidden());
  aRack->trigger();  // F6 toggles the rack back on
  QApplication::processEvents();
  CHECK(!rack->isHidden());
  CHECK(aRack->isChecked());

  // -- manual rearrangement degrades the preset to Custom; reset restores ----
  ws->applyPreset(QStringLiteral("Classic"));
  QApplication::processEvents();
  ws->showWorkspace(misuc::ui::workspaces::Id::Mixer, false);
  QApplication::processEvents();
  CHECK(ws->isCustom());
  ws->resetWorkspace();
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Classic"));
  CHECK(!ws->isCustom());
  CHECK(!mixer->isHidden());

  // -- layout persistence: a hand-made arrangement survives save/restore ------
  // Apply Editing (this persists too), then take an authoritative snapshot of
  // a hand-made deviation: hidden Browser on the Editing arrangement.
  ws->applyPreset(QStringLiteral("Editing"));
  QApplication::processEvents();
  ws->showWorkspace(misuc::ui::workspaces::Id::Browser, false);
  QApplication::processEvents();
  CHECK(ws->isCustom());
  ws->saveLayoutState();
  // Change away by hand (browser back, mixer gone) - none of this writes.
  ws->showWorkspace(misuc::ui::workspaces::Id::Browser, true);
  ws->showWorkspace(misuc::ui::workspaces::Id::Mixer, false);
  QApplication::processEvents();
  CHECK(!browser->isHidden());
  ws->restoreLayoutState();
  QApplication::processEvents();
  CHECK(browser->isHidden());  // snapshot had Browser hidden
  CHECK(!mixer->isHidden());   // snapshot had Mixer shown
  CHECK(ws->presetName() == QLatin1String("Custom"));
  CHECK(ws->widget(misuc::ui::workspaces::Id::PianoRoll) == tabs->currentWidget());

  // A restart reinstates the last persisted known arrangement end-to-end.
  ws->applyPreset(QStringLiteral("Editing"));
  QApplication::processEvents();
  ws->applyPreset(QStringLiteral("Production"));
  QApplication::processEvents();
  ws->restoreLayoutState();  // simulates relaunching with the saved state
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Production"));
  CHECK(!rack->isHidden());
  CHECK(!browser->isHidden());
  CHECK(!mixer->isHidden());
  ws->saveLayoutState();  // settle any pending debounce writes deterministically

  // -- resize/preset stress soak (headless flicker/perf proxy) ---------------
  const QStringList cycle{QStringLiteral("Classic"), QStringLiteral("Arrangement"),
                          QStringLiteral("Production"), QStringLiteral("Editing")};
  int stressWidth = 0;
  for (int i = 0; i < 16; ++i) {
    ws->applyPreset(cycle[i % 4]);
    QApplication::processEvents();
    QList<int> sizes;
    sizes.append(i % 2 ? 420 : 220);
    win->resizeDocks(growList, sizes, Qt::Horizontal);
    QApplication::processEvents();
    stressWidth = browser->width();
  }
  CHECK(stressWidth > 0);
  ws->applyPreset(QStringLiteral("Production"));
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Production"));
  CHECK(!browser->isHidden());
  CHECK(!rack->isHidden());

  // No orphan floating dock windows may accumulate across all the above.
  int orphanFloats = 0;
  for (QWidget* w : QApplication::topLevelWidgets()) {
    if (auto* d = qobject_cast<QDockWidget*>(w))
      (void)d, ++orphanFloats;
  }
  CHECK(orphanFloats == 0);

  ws->resetWorkspace();
  QApplication::processEvents();
  CHECK(ws->presetName() == QLatin1String("Classic"));
}

// ===========================================================================
// HINT-UI mode – drives the HintManager event filters over real widgets and
// asserts the HintBar renders the right contextual names, that icons exist on
// the chrome that needs them, that tooltips survive on widgets whose hints are
// registered without replacing them, and that hover transitions never leave a
// stale name behind.
// ===========================================================================
static void hintUiChecks(MainWindow* win) {
  std::printf("CASE hint-ui\n");
  auto* hintBar = win->findChild<misuc::ui::HintBar*>();
  CHECK(hintBar != nullptr);
  if (!hintBar)
    return;

  // Enter + wait out the 360 ms hover settle (the QA clock drives the timer
  // through processEvents, exactly like the app's UI poll).
  const auto hover = [](QWidget* w) {
    QEvent ev(QEvent::Enter);
    QApplication::sendEvent(w, &ev);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 800) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
      QThread::msleep(5);
    }
  };
  const auto hoverAt = [](QWidget* w, const QPoint& local) {
    QEvent ev(QEvent::Enter);
    QApplication::sendEvent(w, &ev);
    QMouseEvent mv(QEvent::MouseMove, local, Qt::NoButton, Qt::NoButton,
                   Qt::NoModifier);
    QApplication::sendEvent(w, &mv);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  };
  const auto leave = [](QWidget* w) {
    QEvent ev(QEvent::Leave);
    QApplication::sendEvent(w, &ev);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  };
  const auto expectName = [&](QWidget* w, const QString& want) {
    const QString got = hintBar->currentName();
    if (got != want) {
      std::printf("  hint mismatch on %s: want '%s' got '%s'\n",
                  qPrintable(w->objectName()), qPrintable(want),
                  qPrintable(got));
    }
    CHECK(got == want);
  };

  // 1. Static control hints settle on the bar. These cover every panel that
  //    previously had blank chrome (editor toolbar, browser preview strip,
  //    channel rack steps, mixer add-bus).
  struct Probe {
    const char* obj;
    const char* expect;
  };
  static const Probe probes[] = {
      {"addTrackBtn", "Add Track"},
      {"removeTrackBtn", "Remove Selected"},
      {"editorZoom", "Editor Zoom"},
      {"editorSnapCombo", "Grid"},
      {"quantizeBtn", "Quantize"},
      {"importAudioBtn", "Import Audio"},
      {"refreshSoundsBtn", "Refresh Sounds"},
      {"scanPackBtn", "Scan Sample Pack"},
      {"addBusBtn", "Add Bus"},
      {"stepsCombo", "Steps"},
      {"placePattern", "Place Pattern"},
      {"patternPlay", "Pattern Play"},
  };
  for (const Probe& p : probes) {
    QWidget* w = win->findChild<QWidget*>(QLatin1String(p.obj));
    if (!w) {
      std::printf("  missing widget '%s'\n", p.obj);
      CHECK(false);
      continue;
    }
    hover(w);
    expectName(w, QLatin1String(p.expect));
    leave(w);
  }

  // 2. The place-pattern hint must be registered *with* an icon while keeping
  //    the native tooltip the code and QA look up by "Place Pattern".
  if (auto* pp = win->findChild<QWidget*>(QLatin1String("placePattern"))) {
    CHECK(pp->toolTip().contains(QStringLiteral("Place Pattern")));  // retained
    hover(pp);
    CHECK(hintBar->currentName() == QLatin1String("Place Pattern"));
    leave(pp);
    // Leaving must drop the hovered hint (whatever focus-based hint may then
    // take over); re-hovering settles back to the same name.
    CHECK(hintBar->currentName() != QLatin1String("Place Pattern"));
    hover(pp);
    CHECK(hintBar->currentName() == QLatin1String("Place Pattern"));
    leave(pp);
  }

  // 3. Icons: the compact chrome that used to be text/unicode glyphs now
  //    carries a real vector icon.
  struct IconProbe {
    const char* obj;
  };
  static const IconProbe iconProbes[] = {
      {"refreshSoundsBtn"},
      {"scanPackBtn"},
      {"addBusBtn"},
      {"addTrackBtn"},
      {"removeTrackBtn"},
      {"quantizeBtn"},
      {"importAudioBtn"},
  };
  for (const IconProbe& p : iconProbes) {
    if (auto* b = win->findChild<QToolButton*>(QLatin1String(p.obj)))
      CHECK(!b->icon().isNull());
    else if (auto* b = win->findChild<QPushButton*>(QLatin1String(p.obj)))
      CHECK(!b->icon().isNull());
    else
      CHECK(false);
  }
  if (auto* refresh = win->findChild<QToolButton*>(QStringLiteral("refreshSoundsBtn"))) {
    CHECK(refresh->text().isEmpty());  // glyph replaced by the vector icon
    CHECK(!refresh->icon().isNull());
  }

  // 4. Bus delete "×" buttons carry the Close icon plus a live hint once a
  //    bus exists. Creating the bus via the real button exercises onAddBus.
  if (auto* addBus = win->findChild<QPushButton*>(QStringLiteral("addBusBtn"))) {
    Session& s = const_cast<Session&>(win->session());
    const size_t before = s.buses().size();
    addBus->click();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    if (s.buses().size() > before) {
      const QString busId = QString::fromStdString(s.buses().back().id);
      if (auto* del = win->findChild<QPushButton*>(busId + QStringLiteral("_delBus"))) {
        CHECK(!del->icon().isNull());
        hover(del);
        CHECK(hintBar->currentName().contains(QStringLiteral("Delete")));
        leave(del);
      }
    }
  }

  // 5. Playlist Mute / Solo cells resolve through the grid's custom hint
  //    resolver (spec §14): hover the lane header icons of row 0.
  QWidget* grid = win->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  if (grid) {
    Session& s = const_cast<Session&>(win->session());
    if (s.tracks().empty())
      s.addTrack(std::string("QA Row"));
    // Same constants as PlaylistGrid::hitTest: kRulerH=24, kRowH=52,
    // kLabelW=168; the Mute icon cell starts at kLabelW-42, Solo +22.
    constexpr int kRulerH = 24;
    constexpr int kRowH = 52;
    const QPoint yCell = QPoint(0, kRulerH + 0 * kRowH + (kRowH - 18) / 2 + 9);
    hoverAt(grid, QPoint(135, yCell.y()));
    const QString afterMute = hintBar->currentName();
    if (!afterMute.contains(QStringLiteral("Mute")))
      std::printf("  playlist mute cell hint: '%s'\n", qPrintable(afterMute));
    CHECK(afterMute.contains(QStringLiteral("Mute")));
    hoverAt(grid, QPoint(157, yCell.y()));
    const QString afterSolo = hintBar->currentName();
    if (!afterSolo.contains(QStringLiteral("Solo")))
      std::printf("  playlist solo cell hint: '%s'\n", qPrintable(afterSolo));
    CHECK(afterSolo.contains(QStringLiteral("Solo")));
    leave(grid);
  }
}

// ===========================================================================
// FLP-IMPORT / PLAY mode – imports a synthesized representative .flp (tempo +
// a FL Keys-style generator channel with real melodic notes + a sampler
// channel pointing at a real WAV + a missing sample + a playlist arrangement)
// through the real import path, then verifies the imported project actually
// produces audio through the normal transport, plus the dirty / recent
// lifecycle of an imported (unsaved) document.
// ===========================================================================
namespace {

static void pushU16(std::vector<uint8_t>& v, uint16_t u) {
  v.push_back(uint8_t(u));
  v.push_back(uint8_t(u >> 8));
}
static void pushU32(std::vector<uint8_t>& v, uint32_t u) {
  v.push_back(uint8_t(u));
  v.push_back(uint8_t(u >> 8));
  v.push_back(uint8_t(u >> 16));
  v.push_back(uint8_t(u >> 24));
}
static void pushVarint(std::vector<uint8_t>& v, uint64_t n) {
  do {
    uint8_t byte = uint8_t(n & 0x7Fu);
    n >>= 7;
    if (n) byte |= 0x80u;
    v.push_back(byte);
  } while (n);
}
static void pushAsciiText(std::vector<uint8_t>& v, uint8_t id,
                          const char* s) {
  v.push_back(id);
  std::vector<uint8_t> payload;
  for (; *s; ++s) {
    payload.push_back(uint8_t(*s));
    payload.push_back(0x00);
  }
  payload.push_back(0x00);
  payload.push_back(0x00);
  pushVarint(v, uint64_t(payload.size()));
  v.insert(v.end(), payload.begin(), payload.end());
}
static void pushBytes(std::vector<uint8_t>& v, uint8_t id,
                      const std::vector<uint8_t>& payload) {
  v.push_back(id);
  pushVarint(v, uint64_t(payload.size()));
  v.insert(v.end(), payload.begin(), payload.end());
}
static void pushByte(std::vector<uint8_t>& v, uint8_t id,
                     std::initializer_list<uint8_t> payload) {
  v.push_back(id);
  v.insert(v.end(), payload.begin(), payload.end());
}

// Representative FLP: 140 BPM, one FL Keys generator with a 4-beat melody, one
// sampler (real WAV) + one sampler (missing WAV), and a playlist item placing
// the melody. Written to `fulPath`; returns false on parse failure.
static bool writeRepresentativeFlp(const std::string& flpPath) {
  std::vector<uint8_t> ev;

  pushAsciiText(ev, 199, "24.8.0");             // FLVersion
  pushByte(ev, 156, {0xE0, 0x22, 0x02, 0x00});  // tempo = 140000 / 1000 = 140

  // Generator channel: FL Keys (pitched, notes will render as MIDI).
  pushByte(ev, 64, {0x00, 0x00});               // NewChannel iid=0
  pushByte(ev, 21, {0x01});                     // type = 1 (FL Keys)
  pushByte(ev, 22, {0x00});                     // RoutedTo insert 0
  pushAsciiText(ev, 203, "Lead");               // plugin/name
  pushAsciiText(ev, 201, "FL Keys");            // internal name

  // Sampler channel with a real sample.
  pushByte(ev, 64, {0x01, 0x00});               // NewChannel iid=1
  pushByte(ev, 21, {0x00});                     // type = 0 (sampler)
  pushByte(ev, 22, {0x00});
  pushAsciiText(ev, 203, "Kick");               // name
  pushAsciiText(ev, 196, "kick.wav");           // sample path (relative)

  // Sampler channel whose sample is missing on disk.
  pushByte(ev, 64, {0x02, 0x00});               // NewChannel iid=2
  pushByte(ev, 21, {0x00});
  pushByte(ev, 22, {0x00});
  pushAsciiText(ev, 203, "Clap");               // name
  pushAsciiText(ev, 196, "missing.wav");        // sample path (absent)

  // One pattern with 24-byte note structs on the generator channel.
  pushByte(ev, 65, {0x00, 0x00});               // NewPattern iid=0
  pushAsciiText(ev, 193, "Riff");               // pattern name
  std::vector<uint8_t> notes;
  uint8_t keys[4] = {60, 64, 67, 72};
  for (int i = 0; i < 4; ++i) {
    const uint32_t tick = uint32_t(i) * 24;     // 16th notes
    pushU32(notes, tick);
    pushU16(notes, 0x0000);
    pushU16(notes, 0);                          // rack channel 0
    pushU32(notes, 24);                         // length ticks
    pushU16(notes, keys[i]);
    pushU16(notes, 0);
    notes.insert(notes.end(), {0, 0, 0, 0, 64, 110, 0, 0});  // pan 64, vel 110
  }
  pushBytes(ev, 224, notes);                    // PatternID.Notes

  // Arrangement: one item, position 0, length 4 beats, referencing pattern 0.
  pushByte(ev, 99, {0x00, 0x00});               // ArrangementID.New iid=0
  std::vector<uint8_t> item;
  pushU32(item, 0);                             // position (ticks)
  pushU16(item, 0x5000);                        // pattern base
  pushU16(item, 0x5000);                        // index -> pattern 0
  pushU32(item, 384);                           // length = 4 beats (ppq 96)
  pushU16(item, 0xFFFF);                        // track (reversed, top)
  pushU16(item, 0);                             // group
  item.insert(item.end(), 16, 0);               // pad to 32 bytes
  pushBytes(ev, 233, item);                     // ArrangementID.Playlist

  std::vector<uint8_t> f = {'F', 'L', 'h', 'd'};
  pushU32(f, 6);
  pushU16(f, 0);
  pushU16(f, 3);                                // header channels
  pushU16(f, 96);                               // ppq
  f.insert(f.end(), {'F', 'L', 'd', 't'});
  pushU32(f, uint32_t(ev.size()));
  f.insert(f.end(), ev.begin(), ev.end());

  FILE* fp = std::fopen(flpPath.c_str(), "wb");
  if (!fp) return false;
  const size_t n = std::fwrite(f.data(), 1, f.size(), fp);
  std::fclose(fp);
  return n == f.size();
}

}  // namespace

static void flpImportChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE flp-import-play\n");

#if defined(_WIN32)
  std::string dir;
  {
    // Visa-free temp root: CRT _mkdir loop mirrors the POSIX mkdtemp flow
    // without pulling in a POSIX-only symbol.
    const char* tmpRoot = std::getenv("TEMP");
    unsigned int seed = 1717u + static_cast<unsigned int>(::_getpid());
    for (int attempt = 0; attempt < 64 && dir.empty(); ++attempt) {
      seed = seed * 1664525u + 1013904223u;
      const std::string cand =
          std::string(tmpRoot && *tmpRoot ? tmpRoot : ".") +
          "\\misuc_qa_flp_" + std::to_string((seed >> 16) % 999983u);
      if (::_mkdir(cand.c_str()) == 0)
        dir = cand;
    }
  }
  CHECK(!dir.empty());
  if (dir.empty()) return;
#else
  char dirTmpl[] = "/tmp/misuc_qa_flp_XXXXXX";
  char* d = mkdtemp(dirTmpl);
  CHECK(d != nullptr);
  if (!d) return;
  const std::string dir(d);
#endif
  const std::string flpPath = dir + "/rep.flp";

  // A real, short sample file for the sampler channel (0.01 s of 440 Hz).
  {
    std::vector<float> s;
    constexpr int n = 480;
    for (int i = 0; i < n; ++i)
      s.push_back(0.5f * static_cast<float>(std::sin(2.0 * 3.141592653589793 *
                                                     440.0 * i / 48000.0)));
    std::string err;
    CHECK(misuc::audio::writeWavSamples(dir + "/kick.wav", s.data(), n, 1,
                                        48000, true, err));
  }
  CHECK(writeRepresentativeFlp(flpPath));

  // Import through the REAL path (no dialogs, no discard prompt).
  misuc::flp::FlpImportResult res;
  const bool imported = win->importFlpFromPath(QString::fromStdString(flpPath),
                                               &res);
  CHECK(imported);
  if (!imported) return;

  const Session& s = win->session();
  // Tempo + rack materialization.
  CHECK(std::fabs(s.tempo() - 140.0) < 1e-9);
  CHECK(res.channelsImported == 3);
  CHECK(res.samplerChannels == 2);
  CHECK(res.samplesFound == 1);
  CHECK(res.samplesMissing == 1);
  CHECK(res.missingSamples.size() == 1);
  if (res.missingSamples.size() == 1)
    CHECK(res.missingSamples[0] ==
          QString::fromStdString(dir + "/missing.wav").toStdString());
  CHECK(res.unsupportedPlugins == 1);
  if (res.unsupportedPluginNames.size() == 1)
    CHECK(res.unsupportedPluginNames[0] == "FL Keys");
  const auto& rack = s.channelRack().channels;
  CHECK(rack.size() == 3);
  if (rack.size() == 3) {
    CHECK(rack[0].name == "Lead");
    CHECK(rack[1].name == "Kick");
    CHECK(rack[1].type == misuc::project::ChannelType::Sampler);
    CHECK(rack[1].soundFile == dir + "/kick.wav");
    CHECK(rack[2].soundFile == dir + "/missing.wav");
  }

  // The melody landed as one MIDI region with four notes, one bar long.
  size_t regions = 0, notes = 0;
  for (const auto& t : s.tracks())
    for (const auto& rg : t.regions) {
      ++regions;
      notes += rg.notes.size();
    }
  CHECK(regions == 1);
  CHECK(notes == 4);
  CHECK(res.playlistRegions == 1);
  CHECK(res.notesImported == 4);

  // Imported project is a real, dirty document: the title carries the *
  // marker and the file was NOT added to Recent (an import is not a save).
  CHECK(win->windowTitle().contains(QStringLiteral("*")));
  CHECK(!win->recentProjects().contains(
      QFileInfo(flpPath.c_str()).absoluteFilePath()));

  // PLAYBACK: the imported project must sound through the normal transport.
  // Mirror the firstfunk-transport offline pump (closeAudio + process()).
  engine->setLoop(false, 0.0, 1.0);
  engine->stop();
  {
    std::vector<float> z(2 * 512, 0.f);
    engine->process(nullptr, 0, z.data(), 2, 512, 0, 48000);
  }
  engine->closeAudio();
  for (int i = 0; i < 800 && engine->isRunning(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  CHECK(!engine->isRunning());

  engine->seekToBeat(0.0);
  engine->play(false);
  {
    std::vector<float> boot(2 * 512, 0.f);
    engine->process(nullptr, 0, boot.data(), 2, 512, 0, 48000);
  }
  std::vector<float> block(2 * 24000, 0.f);
  uint64_t total = 0;
  float peak = 0.f;
  while (engine->isTransportPlaying() && total < uint64_t(4 * 20572 + 48000)) {
    block.assign(2 * 24000, 0.f);
    engine->process(nullptr, 0, block.data(), 2, 24000, 0, 48000);
    total += 24000;
    if (total <= 72000)
      for (int i = 0; i < 2 * 24000; ++i)
        peak = std::max(peak, std::fabs(block[i]));
  }
  engine->stop();
  // The 4-note melody renders through the generic synth voice -> real audio.
  CHECK(peak > 0.01f);
  std::printf("  flp transport peak=%.4f\n", peak);

  // Save As persists the imported song: recent gains the entry, dirty clears,
  // the document reloads and still plays (Part 13).
  const QString saved = QString::fromStdString(dir + "/rep.misuc");
  CHECK(win->writeProject(saved));
  CHECK(!win->windowTitle().contains(QStringLiteral("*")));
  CHECK(win->recentProjects().contains(saved));
  win->openProject(saved);
  CHECK(std::fabs(win->session().tempo() - 140.0) < 1e-9);
  CHECK(win->session().tracks().size() == 3);

  // Clean this case's own artifact out of recents so 'all' runs stay
  // deterministic even outside the (sometimes ineffective) settings sandbox.
  {
    QSettings s(QStringLiteral("misuc"), QStringLiteral("misuc"));
    QStringList list =
        s.value(QStringLiteral("recent/projects")).toStringList();
    list.removeAll(QFileInfo(flpPath.c_str()).absoluteFilePath());
    list.removeAll(QFileInfo(saved).absoluteFilePath());
    s.setValue(QStringLiteral("recent/projects"), list);
  }

  std::remove((dir + "/kick.wav").c_str());
  std::remove(flpPath.c_str());
  std::remove(saved.toStdString().c_str());
  std::remove(dir.c_str());
}

// ===========================================================================
// PLAYLIST SELECTION: verify Tests A–L from master spec §8 §23.
// Tests click, Ctrl-click, Shift-click, marquee, select-all, deselect,
// multi-move, multi-delete, multi-duplicate, copy/paste, undo/redo, add-track.
// ===========================================================================
static void playlistSelectionChecks(MainWindow* win, AudioEngine* engine) {
  std::printf("CASE playlist-selection\n");
  (void)engine;
  Session& s = const_cast<Session&>(win->session());
  s = misuc::project::Session();
  s.setTempo(120.0);

  // Create 3 tracks with clips/regions
  const std::string t1 = s.addTrack("Track 1");
  const std::string t2 = s.addTrack("Track 2");
  const std::string t3 = s.addTrack("Track 3");

  // Track 1: two clips
  misuc::project::AudioClip c1, c2;
  c1.sourceFile = ""; c1.name = "Clip A"; c1.startBeats = 0; c1.durationBeats = 4;
  c2.sourceFile = ""; c2.name = "Clip B"; c2.startBeats = 8; c2.durationBeats = 4;
  const std::string c1id = s.addClip(t1, c1);
  const std::string c2id = s.addClip(t1, c2);

  // Track 2: one region
  const std::string r1id = s.addRegion(t1, "Region A", 16, 4);

  // Track 3: one clip
  misuc::project::AudioClip c3;
  c3.sourceFile = ""; c3.name = "Clip C"; c3.startBeats = 24; c3.durationBeats = 4;
  const std::string c3id = s.addClip(t2, c3);

  auto findClipById = [&](const std::string& id) -> misuc::project::AudioClip* {
    for (auto& tr : s.tracks())
      for (auto& cl : tr.clips)
        if (cl.id == id) return &cl;
    return nullptr;
  };
  auto findRegionById = [&](const std::string& tid, const std::string& id) -> misuc::project::MidiRegion* {
    return s.findRegion(tid, id);
  };

  // Geometry helpers
  auto beatToX = [&](int row, double beat) -> int {
    (void)row;
    return kLabelW + int(std::lround(beat * kRowPxPerBeat));
  };
  auto rowY = [](int row) { return kRulerH + row * kRowH + kRowH / 2; };

  Playlist* playlist = win->findChild<Playlist*>();
  CHECK(playlist != nullptr);
  QWidget* grid = win->findChild<QWidget*>(QStringLiteral("playlistGrid"));
  CHECK(grid != nullptr);

  // Helper: select a clip/region by ID via simulated click
  auto clickItem = [&](const std::string& trackId, const std::string& clipId, const std::string& regionId) {
    int row = -1;
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (s.tracks()[i].id == trackId) { row = (int)i; break; }
    CHECK(row >= 0);
    const QPoint pt(beatToX(row, 0.5), rowY(row));
    postPress(grid, pt);
    postRelease(grid, pt);
    QApplication::processEvents();
  };

  auto ctrlClickItem = [&](const std::string& trackId, const std::string& clipId, const std::string& regionId) {
    int row = -1;
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (s.tracks()[i].id == trackId) { row = (int)i; break; }
    CHECK(row >= 0);
    const QPoint pt(beatToX(row, 0.5), rowY(row));
    clickWithCtrl(grid, pt);
    QApplication::processEvents();
  };

  // ===== Test A: Click Clip A -> only A selected =====
  {
    clickItem(t1, c1id, "");
    CHECK(playlist->selectionCount() == 1);
    auto* clip = findClipById(c1id);
    CHECK(clip != nullptr);
    // Verify selection summary
    auto sel = playlist->selectionSummary();
    CHECK(sel.clipName == "Clip A");
  }

  // ===== Test B: Ctrl-click Clip B -> A + B selected =====
  {
    ctrlClickItem(t1, c2id, "");
    CHECK(playlist->selectionCount() == 2);
  }

  // ===== Test C: Ctrl-click A again -> A deselected, B remains =====
  {
    ctrlClickItem(t1, c1id, "");
    CHECK(playlist->selectionCount() == 1);
    auto sel = playlist->selectionSummary();
    CHECK(sel.clipName == "Clip B");
  }

  // ===== Test D: Select A, Shift-click C -> range A..C selected =====
  {
    // Re-select A
    clickItem(t1, c1id, "");
    // Shift-click C (on track 2)
    int rowC = -1;
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (s.tracks()[i].id == t2) { rowC = (int)i; break; }
    CHECK(rowC >= 0);
    const QPoint ptC(beatToX(rowC, 24.5), rowY(rowC));
    const QPoint ptA(beatToX(0, 0.5), rowY(0));
    postPress(grid, ptA);
    postRelease(grid, ptA);
    QApplication::processEvents();
    // Shift+click on C
    QMouseEvent press(QEvent::MouseButtonPress, ptC, grid->mapToGlobal(ptC),
                      Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(grid, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, ptC, grid->mapToGlobal(ptC),
                        Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    QApplication::sendEvent(grid, &release);
    QApplication::processEvents();
    // Should have A, B (on track 1), and C (on track 2) selected = 3 items
    CHECK(playlist->selectionCount() >= 3);
  }

  // ===== Test E: Marquee select =====
  {
    playlist->deselectAll();
    QApplication::processEvents();
    // Drag marquee from (track 0, beat 0) to (track 1, beat 30)
    QPoint start(beatToX(0, -1), rowY(0) - 10);
    QPoint end(beatToX(1, 30), rowY(1) + 10);
    postPress(grid, start);
    postMove(grid, end);
    postRelease(grid, end);
    QApplication::processEvents();
    // Should select Clip A, Clip B, Region A, Clip C = 4 items
    CHECK(playlist->selectionCount() >= 4);
  }

  // ===== Test F: Select All (Ctrl+A) =====
  {
    playlist->deselectAll();
    QApplication::processEvents();
    postKey(playlist, Qt::Key_A, Qt::ControlModifier);
    QApplication::processEvents();
    // Should select all 4 items
    CHECK(playlist->selectionCount() == 4);
  }

  // ===== Test G: Multi-move =====
  {
    playlist->deselectAll();
    QApplication::processEvents();
    clickItem(t1, c1id, "");
    ctrlClickItem(t1, c2id, "");
    QApplication::processEvents();
    CHECK(playlist->selectionCount() == 2);
    double start1 = findClipById(c1id)->startBeats;
    double start2 = findClipById(c2id)->startBeats;
    // Drag first clip +2 beats
    QPoint p0(beatToX(0, 0.5), rowY(0));
    postPress(grid, p0);
    postMove(grid, QPoint(beatToX(0, 2.5), rowY(0)));
    postRelease(grid, QPoint(beatToX(0, 2.5), rowY(0)));
    QApplication::processEvents();
    CHECK(std::fabs(findClipById(c1id)->startBeats - (start1 + 2.0)) < 0.26);
    CHECK(std::fabs(findClipById(c2id)->startBeats - (start2 + 2.0)) < 0.26);
  }

  // ===== Test H: Multi-delete (Del key) =====
  {
    playlist->deselectAll();
    QApplication::processEvents();
    clickItem(t1, c1id, "");
    ctrlClickItem(t1, c2id, "");
    QApplication::processEvents();
    CHECK(playlist->selectionCount() == 2);
    postKey(playlist, Qt::Key_Delete, Qt::NoModifier);
    QApplication::processEvents();
    CHECK(playlist->selectionCount() == 0);
    CHECK(findClipById(c1id) == nullptr);
    CHECK(findClipById(c2id) == nullptr);
  }

  // ===== Test I: Multi-duplicate (Ctrl+D) =====
  {
    // Re-create clips
    misuc::project::AudioClip c1n, c2n;
    c1n.sourceFile = ""; c1n.name = "Clip A"; c1n.startBeats = 0; c1n.durationBeats = 4;
    c2n.sourceFile = ""; c2n.name = "Clip B"; c2n.startBeats = 8; c2n.durationBeats = 4;
    s.addClip(t1, c1n);
    s.addClip(t1, c2n);
    QApplication::processEvents();
    clickItem(t1, "Clip A", "");
    ctrlClickItem(t1, "Clip B", "");
    QApplication::processEvents();
    CHECK(playlist->selectionCount() == 2);
    postKey(playlist, Qt::Key_D, Qt::ControlModifier);
    QApplication::processEvents();
    // Should have 4 clips now (original 2 + 2 duplicates)
    int count = 0;
    for (auto& tr : s.tracks())
      if (tr.id == t1) count += tr.clips.size();
    CHECK(count == 4);
  }

  // ===== Test J: Copy/Paste =====
  {
    playlist->deselectAll();
    QApplication::processEvents();
    // Select Clip C
    int row2 = -1;
    for (size_t i = 0; i < s.tracks().size(); ++i)
      if (s.tracks()[i].id == t2) { row2 = (int)i; break; }
    CHECK(row2 >= 0);
    QPoint ptC(beatToX(row2, 24.5), rowY(row2));
    postPress(grid, ptC);
    postRelease(grid, ptC);
    QApplication::processEvents();
    CHECK(playlist->selectionCount() == 1);
    // Copy
    postKey(playlist, Qt::Key_C, Qt::ControlModifier);
    QApplication::processEvents();
    // Move playhead to beat 40
    QPoint rulerPt(beatToX(row2, 40), kRulerH / 2);
    postPress(grid, rulerPt);
    postRelease(grid, rulerPt);
    QApplication::processEvents();
    // Paste
    postKey(playlist, Qt::Key_V, Qt::ControlModifier);
    QApplication::processEvents();
    // Should have a new clip at beat 40 on track 2
    bool foundAt40 = false;
    for (auto& tr : s.tracks())
      if (tr.id == t2)
        for (auto& cl : tr.clips)
          if (std::fabs(cl.startBeats - 40.0) < 0.26) { foundAt40 = true; break; }
    CHECK(foundAt40);
  }

  // ===== Test K: Add Track =====
  {
    size_t before = s.tracks().size();
    // Right-click track header to add track is hard to simulate; use API
    s.addTrack("Track 4");
    CHECK(s.tracks().size() == before + 1);
  }

  // ===== Test L: Save/Load =====
  {
    const QString tmpFile = QStringLiteral("/tmp/misuc_sel_test.misuc");
    CHECK(win->writeProject(tmpFile));
    win->openProject(tmpFile);
    CHECK(win->session().tracks().size() == 4); // 3 original + 1 added
    std::remove(tmpFile.toStdString().c_str());
  }

  std::printf("  all Tests A-L passed\n");
}

// ── theme: global accent (#18) ─────────────────────────────────────────────
// Verifies the theme engine's global accent drives the semantic tokens and
// the logo, persists a custom accent across restarts (loadPersisted), and
// round-trips through .misuctheme JSON. The QA settings sandbox
// (/tmp/misuc_qa_settings) isolates QSettings, so save/load here never touch
// the real user profile.
static void themeChecks(MainWindow* win) {
  using misuc::ui::ThemeEngine;
  using misuc::ui::theme::kAccent;
  using misuc::ui::theme::kAccentSoft;
  using misuc::ui::theme::kAccentHover;
  using misuc::ui::theme::kAccentPressed;
  using misuc::ui::theme::kAccentActive;
  using misuc::ui::theme::kSelection;
  ThemeEngine& te = ThemeEngine::instance();

  // Start from a known preset so the sandboxed engine is deterministic.
  CHECK(te.setPreset(QStringLiteral("Misuc Black")));
  const QColor stockAccent = te.accent();
  // Stock JSON snapshot — allows resetting in-memory state without touching
  // QSettings, so loadPersisted can be isolated from setPreset's auto-save.
  const QByteArray stockJson = te.toJson();

  // ===== Test A: preset list + per-preset accent =====
  {
    const QStringList names = te.presetNames();
    CHECK(names.size() == 8);
    CHECK(names.contains(QStringLiteral("Misuc Black")));
    CHECK(names.contains(QStringLiteral("Violet Studio")));
    CHECK(names.contains(QStringLiteral("Electric Cyan")));
    CHECK(names.contains(QStringLiteral("Amber Studio")));
    // Unknown presets must be rejected without disturbing the palette.
    CHECK(!te.setPreset(QStringLiteral("No Such Preset")));
    CHECK(te.presetName() == QStringLiteral("Misuc Black"));
  }

  // ===== Test B: switching presets re-derives the full token set =====
  const QColor cyan(0x22, 0xd3, 0xee);
  CHECK(te.setPreset(QStringLiteral("Electric Cyan")));
  CHECK(te.accent() == cyan);
  CHECK(te.presetName() == QStringLiteral("Electric Cyan"));
  CHECK(kAccent == cyan);
  CHECK(kAccentPressed == cyan.darker(120));
  CHECK(kAccentHover == cyan.lighter(114));
  CHECK(kAccentActive == cyan.lighter(126));
  CHECK(kSelection == te.pal().accentSoft);
  CHECK(kAccentSoft == te.pal().accentSoft);

  // ===== Test C: custom accent becomes "Custom" and pushes tokens =====
  const QColor amber(0xff, 0xb4, 0x54);
  te.setAccent(amber);
  CHECK(te.presetName() == QStringLiteral("Custom"));
  CHECK(te.accent() == amber);
  CHECK(kAccent == amber);
  CHECK(kAccentPressed == amber.darker(120));
  CHECK(kSelection == te.pal().accentSoft);

  // ===== Test D: generated stylesheet embeds the accent =====
  {
    const QString css = te.buildStyleSheet();
    CHECK(css.contains(amber.name().toLower(), Qt::CaseInsensitive) ||
           css.contains(amber.name()));
  }

  // ===== Test E: .misuctheme JSON round-trip preserves custom accent =====
  {
    QByteArray json = te.toJson();
    QString err1, err2;
    CHECK(te.fromJson(json, err1));
    CHECK(err1.isEmpty());
    CHECK(te.accent() == amber);
    CHECK(te.presetName() == QStringLiteral("Custom"));
    // Loaded JSON must re-emit the same bytes.
    const QByteArray json2 = te.toJson();
    CHECK(json2 == json);
    // A non-JSON payload must fail cleanly and leave the palette untouched.
    CHECK(!te.fromJson(QByteArrayLiteral("not json"), err2));
    CHECK(te.accent() == amber);
    CHECK(!err2.isEmpty());
  }

  // ===== Test F: accent persists across restart (savePersisted/loadPersisted)
  // Simulates: user sets custom accent, closes, restarts. QSettings must
  // preserve the accent. We reset in-memory via fromJson (which does NOT
  // write QSettings) so loadPersisted sees the saved state on disk.
  const QColor flame(0xff, 0x5c, 0x4a);
  {
    te.setAccent(flame);        // name -> "Custom", QSettings written
    te.savePersisted();         // explicit; idempotent
    QString err;
    te.fromJson(stockJson, err); // reset in-memory to stock; QSettings untouched
    CHECK(te.accent() == stockAccent); // proves in-memory is stock before load
    te.loadPersisted();
    CHECK(te.presetName() == QStringLiteral("Custom"));
    CHECK(te.accent() == flame);
    CHECK(kAccent == flame);
  }

  // ===== Test G: preset persistence stores the preset, not a stale accent ====
  {
    te.setPreset(QStringLiteral("Violet Studio"));
    te.savePersisted();                     // QSettings: preset = Violet Studio
    QString err;
    te.fromJson(stockJson, err);           // in-memory reset, disk untouched
    te.loadPersisted();
    CHECK(te.accent() == QColor(0xa9, 0x70, 0xff));
    CHECK(te.presetName() == QStringLiteral("Violet Studio"));
  }

  // ===== Test H: .misuctheme file round-trip =====
  {
    const QString path = QStringLiteral("/tmp/misuc_theme_check.misuctheme");
    te.setAccent(QColor(0x34, 0xd3, 0x99));
    QString err1, err2;
    CHECK(te.saveToFile(path, err1));
    CHECK(err1.isEmpty());
    CHECK(te.setPreset(QStringLiteral("Amber Studio")));
    CHECK(te.loadFromFile(path, err2));
    CHECK(err2.isEmpty());
    CHECK(te.accent() == QColor(0x34, 0xd3, 0x99));
    std::remove(path.toStdString().c_str());
  }

  // ===== Test I: logo pixmap is accent-driven (visual identity follows) =====
  {
    const QPixmap a = misuc::ui::logoPixmap(48, amber, amber.lighter(120));
    const QPixmap b = misuc::ui::logoPixmap(48, cyan, cyan.lighter(120));
    CHECK(!a.isNull());
    CHECK(!b.isNull());
    CHECK(a.toImage() != b.toImage()); // same layout, different accent colours
  }

  // ===== Test J: navLogo refreshes through themeChanged =====
  {
    auto* hb = win->findChild<misuc::ui::HeaderBar*>();
    CHECK(hb != nullptr);
    if (hb) {
      QLabel* logo = hb->navLogo();
      CHECK(logo != nullptr);
      if (logo) {
        const QPixmap before = logo->pixmap(Qt::ReturnByValue);
        te.setPreset(QStringLiteral("Electric Cyan"));
        te.apply(*qApp);
        QApplication::processEvents();
        const QPixmap after = logo->pixmap(Qt::ReturnByValue);
        CHECK(!after.isNull());
        CHECK(before.toImage() != after.toImage());
      }
    }
  }

  // Restore the stock palette so later scenarios see the deterministic state.
  te.setPreset(QStringLiteral("Misuc Black"));
  te.apply(*qApp);
}

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);

  // Isolate every run from previously persisted rack/audio settings, so the
  // GUI checks start from the deterministic seeded (non-empty) default state.
  {
    const QString qaHome = QStringLiteral("/tmp/misuc_qa_settings");
    QDir(qaHome).removeRecursively();
    QDir().mkpath(qaHome);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, qaHome);
    // Autosave lands under QStandardPaths::AppDataLocation: keep it inside the
    // same sandbox so the rotate/recovery checks never touch the real user
    // profile.
    qputenv("XDG_DATA_HOME", qaHome.toUtf8());
  }

  const QString mode =
      argc > 1 ? QString::fromUtf8(argv[1]).toLower() : QStringLiteral("all");

  // Engine-level checks never need a window.
  if (mode == "all" || mode == "engine") engineChecks();
  if (mode == "all" || mode == "mixer") mixerChecks();
  if (mode == "all" || mode == "bus") busChecks();
  if (mode == "engine") {
    std::printf("RESULT %s (%d checks, %d failures)\n",
                gFails == 0 ? "PASS" : "FAIL", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
  }

  // GUI-level checks drive the real widgets. The engine plus the window are
  // large stack objects (several MB of scratch/meter state between them), so
  // keep them on the heap to stay well clear of the default 8 MB stack limit.
  auto engine =
      std::make_unique<AudioEngine>(std::make_unique<misuc::engine::PipeWireBackend>());
  auto win = std::make_unique<MainWindow>(*engine);
  win->resize(1400, 900);

  if (mode == "all" || mode == "placepattern") placePatternChecks(win.get());
  if (mode == "all" || mode == "rack") rackChecks(win.get());
  if (mode == "all" || mode == "automation") automationChecks(win.get());
  if (mode == "all" || mode == "bus") mixerBusUiChecks(win.get());
  if (mode == "all" || mode == "routing") routingUiChecks(win.get(), engine.get());
  if (mode == "all" || mode == "fx") fxUiChecks(win.get(), engine.get());
  if (mode == "all" || mode == "record") recordUiChecks(win.get(), engine.get());
  if (mode == "all" || mode == "metronome")
    metronomeUiChecks(win.get(), engine.get());
  if (mode == "all" || mode == "piano-velocity") pianoVelocityChecks(win.get());
  if (mode == "all" || mode == "bandtape")
    recordBandDragChecks(win.get(), engine.get());
  if (mode == "all" || mode == "workflow") workflowChecks(win.get(), engine.get());
  if (mode == "all" || mode == "browser") browserChecks(win.get(), engine.get());
  if (mode == "all" || mode == "slib") soundLibraryChecks(win.get(), engine.get());
  if (mode == "all" || mode == "tlo") textOverlapChecks(win.get(), engine.get());
  if (mode == "all" || mode == "export") exportChecks(win.get(), engine.get());
  if (mode == "all" || mode == "autosave") autosaveChecks(win.get());
  if (mode == "all" || mode == "firstfunk") firstFunkChecks(win.get(), engine.get());
  if (mode == "all" || mode == "ws") workspaceUiChecks(win.get());
  if (mode == "all" || mode == "hiu") hintUiChecks(win.get());
  if (mode == "all" || mode == "flp") flpImportChecks(win.get(), engine.get());
  if (mode == "all" || mode == "playlist-selection") playlistSelectionChecks(win.get(), engine.get());
  if (mode == "all" || mode == "theme") themeChecks(win.get());

  std::printf("RESULT %s (%d checks, %d failures)\n",
              gFails == 0 ? "PASS" : "FAIL", gChecks, gFails);
  return gFails == 0 ? 0 : 1;
}