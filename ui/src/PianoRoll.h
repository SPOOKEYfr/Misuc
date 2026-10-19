#pragma once

#include <QWidget>
#include <QString>
#include <functional>
#include <vector>

#include <misuc/engine/project/Session.h>

class QScrollArea;
class QScrollBar;
class QMouseEvent;
class QShowEvent;

namespace misuc::ui {

// Ruler above the piano-roll canvas; scrolls horizontally with it. Also shows
// the loop region, the currently edited region's name, and lets you draw a
// loop by dragging. A plain click seeks the transport caret (onSeekClicked).
class PianoRuler : public QWidget {
  Q_OBJECT
public:
  explicit PianoRuler(QWidget* parent = nullptr);
  void setHorizon(double numBeats, int pxPerBeat, int barBeats, int hbar);
  void setPlayheadBeat(double beats, bool visible);
  void setLoop(bool on, double startBeats, double endBeats);
  void setRegionName(const QString& name);
  // Callback for a completed loop-region drag (normalized start/end beats).
  std::function<void(double, double)> onLoopDrawn;
  // Callback for a plain left-click seek (beat in ruler space).
  std::function<void(double)> onSeekClicked;

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  double beatAtX(double x) const;

  double numBeats_ = 64.0;
  int pxPerBeat_ = 40;
  int barBeats_ = 4;
  int hbar_ = 0;
  double playheadBeats_ = -1.0;
  bool playheadVisible_ = false;
  bool loopOn_ = false;
  double loopStartBeats_ = 0.0;
  double loopEndBeats_ = 4.0;
  bool grabbingLoop_ = false;
  bool moved_ = false;
  double grabStartBeat_ = 0.0;
  double grabEndBeat_ = 0.0;
  QString regionName_;
};

// Piano-key column left of the canvas; scrolls vertically with it. Clicking or
// dragging a key auditions that pitch through the piano-roll owner.
class PianoKeys : public QWidget {
  Q_OBJECT
public:
  explicit PianoKeys(QWidget* parent = nullptr);
  void setRange(int lowPitch, int rowH, int vbar);
  QSize sizeHint() const override;

signals:
  void keyPressed(int pitch);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

private:
  int pitchAtY(int y) const;

  int lowPitch_ = 0;
  int rowH_ = 18;
  int vbar_ = 0;
};

// A real MIDI piano-roll editor bound to one track of a shared Session.
//  - Top ruler (bars/beats), left piano keyboard, scrollable note grid.
//  - Left-click a note and drag to move it (time + pitch); drag the note's
//    right edge to resize its duration (snapped).
//  - Left-double-click empty space adds a note (snapped); double-clicking a
//    note deletes it; right-click deletes; Del removes the selection.
//  - Wheel over a note adjusts its velocity (auditioned live, committed once
//    the scroll gesture settles); Ctrl+wheel zooms.
//  - Each note draws a velocity bar (height proportional to velocity).
//  - Left-drag on empty space rubber-bands a selection; all selected notes
//    move together when any one is dragged, and Del/Backspace deletes them all
//    as a single undo step.
//  - Ctrl+C copies the selection, Ctrl+V pastes it (at the playhead when one is
//    shown, else one grid step to the right of the copy origin), and Ctrl+D
//    duplicates it right next to itself — each paste is one undo snapshot.
//  - The ruler shows a green loop region; drag across the ruler to set it
//    (snapped to the grid, persisted with the project, live in the engine).
//  - Horizontal zoom (px per beat) and snap-to-sixteenth toggle.
// Edits mutate the shared Session directly and emit edited() exactly once per
// committed gesture so the main window can push an undo snapshot.
class PianoRoll : public QWidget {
  Q_OBJECT
public:
  explicit PianoRoll(misuc::project::Session& session, QWidget* parent = nullptr);

  void setTrack(const QString& id);
  void setRegion(const QString& trackId, const QString& regionId);
  QString trackId() const { return trackId_; }
  QString regionId() const { return regionId_; }
  QString regionName() const;
  void refresh(); // repaint from the session
  // Current vertical pixel offset of the note-grid scroll (0 = highest pitch).
  int verticalScroll() const { return vbarValue_; }
  void setPlayhead(bool playing, double beats);
  void autoFollow(bool playing, double beats);

  void setZoom(double pxPerBeat);
  double zoom() const { return pxPerBeat_; }
  // Vertical zoom: row height in pixels per semitone (default kRowH=18,
  // 12..48). Scrolling always spans the full MIDI 0..127 range.
  void setRowHeight(int rowH);
  int rowHeight() const { return rowH_; }
  void setSnap(bool on) { snap_ = on; }
  bool snap() const { return snap_; }
  // Snap-grid resolution in beats (1 beat = a quarter note). 1/16 -> 0.25.
  // Supported values are those returned by snapBeatsOptions(). 0 disables.
  void setSnapBeats(double beats) { snapBeats_ = beats; }
  double snapBeats() const { return snapBeats_; }
  // The selectable snap resolutions (1/32, 1/16, 1/8, 1/4, 1/2, 1).
  static std::vector<double> snapBeatsOptions() {
    return {0.03125, 0.0625, 0.125, 0.25, 0.5, 1.0};
  }
  // Snaps the selection (or all notes) to the grid; one undo snapshot.
  void quantize();
  // Velocity tool: nudges the selection's velocities by `delta` (clamped to
  // 1..127) in one undo snapshot. No-op without a selection.
  void nudgeVelocity(int delta);
  // Mirror the session's persisted loop region into the ruler.
  void setLoop(bool on, double startBeats, double endBeats);

  // Ruler geometry / editing scope.
  double horizonBeats() const { return horizonBeats_; }
  void ensureRegion(); // selects (or creates) a real region for current track

  // Ruler/keyboard/canvas geometry shared with the drawing code.
  static constexpr int kKeyWidth = 60;
  static constexpr int kRulerH = 26;
  static constexpr int kClipLaneH = 28; // audio-clip lane above the note grid
  static constexpr int kRowH = 18;
  static constexpr int kLowPitch = 0;       // full MIDI range (0..127)
  static constexpr int kRowCount = 128;
  static constexpr double kHorizonBeats = 64.0;
  static constexpr int kVelLaneH = 56;      // velocity lane below the notes

signals:
  void edited();
  // Audition feedback: freq in Hz, velocity 0..1. Emitted as notes are added,
  // dragged, or clicked so the UI can preview them even while stopped.
  void audition(double freq, float velocity);
  // Transport seek requested by clicking the ruler (global beats).
  void seekRequested(double beat);

protected:
  void paintEvent(QPaintEvent* e) override;
  QSize sizeHint() const override;
  void showEvent(QShowEvent* e) override;

private:
  class Canvas;
  friend class Canvas;
  friend class PianoRuler;
  friend class PianoKeys;

  void commit() noexcept { emit edited(); }

  // Region-frame helpers: local coordinates are region-relative (0 at the
  // region's own start); global coordinates are transport/timeline positions.
  double regionStart() const;
  double localBeat(double globalBeat) const;
  double toGlobalBeat(double localBeat) const;

  misuc::project::Session& session_;
  QString trackId_;
  QString regionId_;
  double horizonBeats_ = kHorizonBeats;
  Canvas* canvas_ = nullptr;
  PianoRuler* ruler_ = nullptr;
  PianoKeys* keys_ = nullptr;
  QScrollArea* scroller_ = nullptr;
  QScrollBar* hbar_ = nullptr;
  QScrollBar* vbar_ = nullptr;

  double pxPerBeat_ = 40.0;
  int rowH_ = kRowH;
  bool snap_ = true;
  double snapBeats_ = 0.25;  // 1/16 note, the protocol default grid
  int hbarValue_ = 0;
  int vbarValue_ = 0;
  double playheadBeats_ = -1.0;
  bool playheadPlaying_ = false;
  bool centerMiddleOnce_ = false; // first show() snaps the view to middle C
};

} // namespace misuc::ui