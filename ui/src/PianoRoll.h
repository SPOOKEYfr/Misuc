#pragma once

#include <QWidget>
#include <QString>

#include <misuc/engine/project/Session.h>

class QScrollArea;
class QScrollBar;

namespace misuc::ui {

// Ruler above the piano-roll canvas; scrolls horizontally with it.
class PianoRuler : public QWidget {
  Q_OBJECT
public:
  explicit PianoRuler(QWidget* parent = nullptr);
  void setHorizon(double numBeats, int pxPerBeat, int barBeats, int hbar);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  double numBeats_ = 64.0;
  int pxPerBeat_ = 40;
  int barBeats_ = 4;
  int hbar_ = 0;
};

// Piano-key column left of the canvas; scrolls vertically with it.
class PianoKeys : public QWidget {
  Q_OBJECT
public:
  explicit PianoKeys(QWidget* parent = nullptr);
  void setRange(int lowPitch, int rowH, int vbar);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  int lowPitch_ = 24;
  int rowH_ = 18;
  int vbar_ = 0;
};

// A real MIDI piano-roll editor bound to one track of a shared Session.
//  - Top ruler (bars/beats), left piano keyboard, scrollable note grid.
//  - Left-click a note and drag to move it (time + pitch).
//  - Left-double-click empty space adds a note (snapped); double-clicking a
//    note deletes it; right-click deletes; Del removes the selection.
//  - Horizontal zoom (px per beat) and snap-to-sixteenth toggle.
// Edits mutate the shared Session directly and emit edited() exactly once per
// committed gesture so the main window can push an undo snapshot.
class PianoRoll : public QWidget {
  Q_OBJECT
public:
  explicit PianoRoll(misuc::project::Session& session, QWidget* parent = nullptr);

  void setTrack(const QString& id);
  QString trackId() const { return trackId_; }
  void refresh(); // repaint from the session

  void setZoom(double pxPerBeat);
  double zoom() const { return pxPerBeat_; }
  void setSnap(bool on) { snap_ = on; }
  bool snap() const { return snap_; }

  // Ruler/keyboard/canvas geometry shared with the drawing code.
  static constexpr int kKeyWidth = 60;
  static constexpr int kRulerH = 26;
  static constexpr int kRowH = 18;
  static constexpr int kLowPitch = 24; // C1
  static constexpr int kRowCount = 61; // pitches 24..84 (C1..C6)
  static constexpr double kHorizonBeats = 64.0;

signals:
  void edited();

protected:
  void paintEvent(QPaintEvent* e) override;
  QSize sizeHint() const override;

private:
  class Canvas;
  friend class Canvas;
  friend class PianoRuler;
  friend class PianoKeys;

  void commit() noexcept { emit edited(); }

  misuc::project::Session& session_;
  QString trackId_;
  Canvas* canvas_ = nullptr;
  PianoRuler* ruler_ = nullptr;
  PianoKeys* keys_ = nullptr;
  QScrollArea* scroller_ = nullptr;
  QScrollBar* hbar_ = nullptr;
  QScrollBar* vbar_ = nullptr;

  double pxPerBeat_ = 40.0;
  bool snap_ = true;
  int hbarValue_ = 0;
  int vbarValue_ = 0;
};

} // namespace misuc::ui