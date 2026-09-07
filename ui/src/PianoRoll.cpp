#include "PianoRoll.h"

#include <algorithm>
#include <cmath>

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Theme.h"

#include <misuc/engine/dsp/Midi.h>

using misuc::project::NoteEvent;
using misuc::project::Track;

namespace misuc::ui {

namespace {

constexpr double kSnapBeats = 0.25; // 1/16 note
constexpr int kHighPitch = PianoRoll::kLowPitch + PianoRoll::kRowCount - 1;
constexpr int kVelocityStep = 127 / 25; // one wheel notch
constexpr int kVelocityCommitMs = 280;  // undo-snapshot debounce
constexpr double kResizeGrabPx = 6.0;   // right-edge grab band, px
constexpr double kMinDurationBeats = 0.1;

int pitchRowY(int pitch, int rowH) { return (kHighPitch - pitch) * rowH; }

int yToPitch(int y, int rowH) {
  return std::clamp(kHighPitch - y / rowH, PianoRoll::kLowPitch, kHighPitch);
}

bool isBlackKey(int pitch) {
  const int c = pitch % 12;
  return c == 1 || c == 3 || c == 6 || c == 8 || c == 10;
}

QString pitchLabel(int pitch) {
  static const char* names[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                "F#", "G",  "G#", "A",  "A#", "B"};
  return QStringLiteral("%1%2")
      .arg(names[pitch % 12])
      .arg(pitch / 12 - 1);
}

} // namespace

// --- Ruler ---------------------------------------------------------------

PianoRuler::PianoRuler(QWidget* parent) : QWidget(parent) {
  setFixedHeight(PianoRoll::kRulerH);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void PianoRuler::setPlayheadBeat(double beats, bool visible) {
  playheadBeats_ = beats;
  playheadVisible_ = visible;
  update();
}

void PianoRuler::setHorizon(double numBeats, int pxPerBeat, int barBeats,
                            int hbar) {
  numBeats_ = numBeats;
  pxPerBeat_ = pxPerBeat;
  barBeats_ = barBeats;
  hbar_ = hbar;
  update();
}

void PianoRuler::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::kPanel);
  p.setPen(QPen(theme::kBorder, 1));
  p.drawLine(0, height() - 1, width(), height() - 1);

  const double px = static_cast<double>(pxPerBeat_);
  if (barBeats_ <= 0 || px <= 0.0)
    return;

  const double x0 = -hbar_;
  const double x1 = x0 + width();
  const int firstBar = std::max(0, static_cast<int>(std::floor(x0 / (barBeats_ * px))));

  for (int b = firstBar;; ++b) {
    const double x = b * barBeats_ * px - hbar_;
    if (x > x1 + 2)
      break;
    p.setPen(QPen(theme::kBorderBright, 1));
    p.drawLine(QPointF(x, 2), QPointF(x, height() - 2));
    p.setPen(theme::kTextDim);
    p.setFont(theme::monoFont(8));
    p.drawText(QRectF(x + 4, 0, 60, height() - 3), Qt::AlignVCenter | Qt::AlignLeft,
               QString::number(b + 1));
  }

  // Beat ticks between bars.
  for (int bar = firstBar;; ++bar) {
    const double barX = bar * barBeats_ * px - hbar_;
    if (barX > x1 + 2)
      break;
    for (int beat = 1; beat < barBeats_; ++beat) {
      const double x = barX + beat * px;
      p.setPen(QPen(theme::kBorder, 1));
      p.drawLine(QPointF(x, height() - 8), QPointF(x, height() - 1));
    }
  }

  // Playhead caret aligned with the canvas line below.
  if (playheadVisible_ && playheadBeats_ >= 0.0) {
    const double cx = playheadBeats_ * px - hbar_;
    if (cx >= -1.0 && cx <= width() + 1.0) {
      p.setPen(QPen(theme::kOk, 2));
      p.drawLine(QPointF(cx, 2), QPointF(cx, height() - 1));
    }
  }
}

// --- Keyboard ------------------------------------------------------------

PianoKeys::PianoKeys(QWidget* parent) : QWidget(parent) {
  setFixedWidth(PianoRoll::kKeyWidth);
}

QSize PianoKeys::sizeHint() const {
  return QSize(PianoRoll::kKeyWidth, PianoRoll::kRowCount * PianoRoll::kRowH);
}

void PianoKeys::setRange(int lowPitch, int rowH, int vbar) {
  lowPitch_ = lowPitch;
  rowH_ = rowH;
  vbar_ = vbar;
  update();
}

void PianoKeys::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::kPanelRaised);
  const int y0 = -vbar_;
  const int y1 = y0 + height();
  for (int pitch = kHighPitch; pitch >= lowPitch_; --pitch) {
    const int y = pitchRowY(pitch, rowH_) - vbar_;
    if (y + rowH_ < y0 || y > y1)
      continue;
    const QRect row(0, y, width() - 1, rowH_ - 1);
    if (isBlackKey(pitch)) {
      p.fillRect(row, theme::kPanelSunken);
      p.setPen(QPen(theme::kBorder, 1));
      p.drawRect(row.adjusted(0, 0, -1, -1));
    } else {
      p.fillRect(row, QColor(0x232c3b));
      p.setPen(QPen(theme::kBorder, 1));
      p.drawRect(row.adjusted(0, 0, -1, -1));
    }
    if (pitch % 12 == 0) { // label every C
      p.setPen(theme::kTextDim);
      p.setFont(theme::monoFont(8));
      p.drawText(row.adjusted(7, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                 pitchLabel(pitch));
      if (pitch == 60) { // middle C accent
        p.setPen(theme::kAccent);
        p.drawLine(0, y + 0, width() - 1, y + 0);
      }
    }
  }
}

// --- Canvas --------------------------------------------------------------

class PianoRoll::Canvas : public QWidget {
public:
  explicit Canvas(PianoRoll* owner) : QWidget(owner), owner_(owner) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(
        static_cast<int>(PianoRoll::kHorizonBeats * owner_->pxPerBeat_) + 1,
        PianoRoll::kRowCount * PianoRoll::kRowH + 1);
    // Wheel velocity edits are committed once the gesture settles so a single
    // scroll burst produces exactly one undo snapshot.
    velocityTimer_.setSingleShot(true);
    connect(&velocityTimer_, &QTimer::timeout, this, [this] { owner_->commit(); });
  }

  QSize sizeHint() const override { return size(); }

  double xToBeat(double x) const { return x / owner_->pxPerBeat_; }
  double beatToX(double beat) const { return beat * owner_->pxPerBeat_; }
  int yToPitch(int y) const { return ::misuc::ui::yToPitch(y, owner_->kRowH); }
  int pitchToY(int pitch) const {
    return ::misuc::ui::pitchRowY(pitch, owner_->kRowH);
  }
  double snapped(double beat) const {
    if (!owner_->snap_)
      return beat;
    return std::round(beat / kSnapBeats) * kSnapBeats;
  }

  void setPlayheadBeat(double beats, bool visible) {
    playheadBeats_ = beats;
    playheadVisible_ = visible;
    update();
  }

  const Track* track() const { return owner_->session_.findTrack(owner_->trackId_.toStdString()); }
  Track* trackEdit();

  int hitTest(const QPointF& pos) const {
    const Track* t = track();
    if (!t)
      return -1;
    for (size_t i = 0; i < t->notes.size(); ++i) {
      if (noteRect(t->notes[i]).adjusted(0, 2, 0, -2).contains(pos))
        return static_cast<int>(i);
    }
    return -1;
  }

  // True when a press/click lands in the right-edge grab band of a note.
  bool onResizeGrip(const QPointF& pos) const {
    const Track* t = track();
    if (!t)
      return false;
    for (size_t i = 0; i < t->notes.size(); ++i) {
      const QRectF r = noteRect(t->notes[i]);
      if (r.adjusted(0, 2, 0, -2).contains(pos) &&
          pos.x() >= r.right() - kResizeGrabPx)
        return true;
    }
    return false;
  }

  QRectF noteRect(const NoteEvent& n) const {
    const double x = beatToX(n.startBeats);
    const double w = std::max<double>(3.0, n.durationBeats * owner_->pxPerBeat_ - 1.0);
    const int y = pitchToY(n.pitch);
    return QRectF(x, y, w, owner_->kRowH - 1);
  }

  void addNoteAt(double beat, int pitch) {
    Track* t = trackEdit();
    if (!t)
      return;
    NoteEvent n;
    n.pitch = static_cast<uint8_t>(pitch);
    n.velocity = 100;
    n.startBeats = snapped(beat);
    n.durationBeats = 0.5;
    const int idx = static_cast<int>(t->notes.size());
    t->notes.push_back(n);
    selected_ = idx;
    update();
    owner_->audition(misuc::dsp::midiNoteToFreq(static_cast<double>(pitch)),
                    100.0f / 127.0f);
  }

  bool isSelected(int index) const {
    return index == selected_ ||
           std::find(selectSet_.begin(), selectSet_.end(), index) !=
               selectSet_.end();
  }

  // Removes the given (descending) indices and invalidates all selection
  // state; callers commit once afterward.
  void deleteNotes(std::vector<int> indices) {
    Track* t = trackEdit();
    if (!t)
      return;
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (const int i : indices) {
      if (i < 0 || i >= static_cast<int>(t->notes.size()))
        continue;
      t->notes.erase(t->notes.begin() + i);
    }
    selected_ = -1;
    selectSet_.clear();
    update();
  }

  void deleteNote(int index) {
    deleteNotes({index});
    update();
  }

  struct DragSnap {
    int index;
    double start;
    int pitch;
  };

  bool selecting_ = false; // rubber-band selection in progress
  QPointF marquee0_;
  QPointF marquee1_;
  bool multimove_ = false; // dragging a whole multi-selection
  std::vector<DragSnap> groupSnap_;
  std::vector<int> selectSet_;
  int selected_ = -1;
  bool dragging_ = false;
  bool dragged_ = false;
  int dragIndex_ = -1;
  double dragOffsetBeats_ = 0.0;
  int dragOffsetPitch_ = 0;
  double grabBeat_ = 0.0;
  int grabPitch_ = 0;
  bool resizing_ = false;
  bool resized_ = false;
  int resizeIndex_ = -1;
  double playheadBeats_ = -1.0;
  bool playheadVisible_ = false;
  QTimer velocityTimer_;

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanelSunken);
    const Track* t = track();
    const int barBeats = owner_->session_.timeSigNumerator();

    // Vertical grid: sixteenths faint, quarters normal, bars strong.
    const int lastBeat = static_cast<int>(PianoRoll::kHorizonBeats);
    for (int b = 0; b <= lastBeat; ++b) {
      const double x = beatToX(b);
      const bool bar = barBeats > 0 && (b % barBeats == 0);
      const bool beat = b % 4 == 0;
      p.setPen(QPen(bar ? theme::kBorderBright
                       : beat ? theme::kMeterTrack : theme::kMeterDim,
                    1));
      p.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
    // Horizontal pitch lines every octave.
    for (int pitch = kLowPitch; pitch <= kHighPitch; pitch += 12) {
      const int y = pitchToY(pitch) - 1;
      p.setPen(QPen(theme::kMeterTrack, 1));
      p.drawLine(QPointF(0, y), QPointF(width(), y));
    }

    if (!t) {
      p.setPen(theme::kTextFaint);
      p.drawText(rect(), Qt::AlignCenter,
                 QStringLiteral("Select or add a track to edit notes"));
      return;
    }

    for (size_t i = 0; i < t->notes.size(); ++i) {
      const NoteEvent& n = t->notes[i];
      const QRectF r = noteRect(n);
      QColor fill = theme::kAccent;
      const int a = 90 + static_cast<int>(n.velocity * 110 / 127);
      fill.setAlpha(std::min(230, a));
      p.setBrush(fill);
      p.setPen(QPen(isSelected(static_cast<int>(i)) ? theme::kOk
                                                    : theme::kBorderBright,
                    1));
      p.drawRoundedRect(r.adjusted(0, 0, -1, -1), 2, 2);
      // Velocity bar: vertical bar growing from the note floor, height
      // proportional to how hard the note is struck.
      const double vh =
          (static_cast<double>(n.velocity) / 127.0) * (owner_->kRowH - 8);
      p.setPen(Qt::NoPen);
      p.setBrush(theme::kOk);
      p.drawRect(
          QRectF(r.left() + 3.0, r.bottom() - 1.0 - vh, 3.0, vh));
      p.setBrush(fill);
    }

    // Playhead caret, drawn above the notes so it stays visible.
    if (playheadVisible_ && playheadBeats_ >= 0.0) {
      const double cx = beatToX(playheadBeats_);
      if (cx >= -1.0 && cx <= width() + 1.0) {
        p.setPen(QPen(theme::kOk, 2));
        p.drawLine(QPointF(cx, 0), QPointF(cx, height()));
      }
    }

    // Rubber-band selection marquee.
    if (selecting_) {
      QRectF box(marquee0_, marquee1_);
      box = box.normalized();
      p.setPen(QPen(QColor(0x9c, 0xa3, 0xaf), 1, Qt::DashLine));
      p.setBrush(QColor(0x4a, 0xde, 0x80, 40));
      p.drawRect(box);
    }
  }

  void mousePressEvent(QMouseEvent* e) override {
    setFocus();
    const QPointF pos = e->position();
    if (e->button() == Qt::LeftButton) {
      if (onResizeGrip(pos)) {
        const int hit = hitTest(pos);
        selected_ = hit;
        selectSet_.clear();
        resizeIndex_ = hit;
        resizing_ = true;
        resized_ = false;
        owner_->audition(misuc::dsp::midiNoteToFreq(
                             static_cast<double>(track()->notes[hit].pitch)),
                         track()->notes[hit].velocity / 127.0f);
        update();
        return;
      }
      const int hit = hitTest(pos);
      if (hit >= 0) {
        const Track* t = track();
        const NoteEvent& n = t->notes[hit];
        multimove_ = !selectSet_.empty() && isSelected(hit);
        selected_ = hit;
        dragIndex_ = hit;
        grabBeat_ = xToBeat(pos.x());
        grabPitch_ = yToPitch(pos.y());
        dragOffsetBeats_ = grabBeat_ - n.startBeats;
        dragOffsetPitch_ = grabPitch_ - static_cast<int>(n.pitch);
        dragging_ = true;
        dragged_ = false;
        if (multimove_) {
          groupSnap_.clear();
          auto snapshot = [&](int i) {
            groupSnap_.push_back(
                {i, t->notes[i].startBeats, static_cast<int>(t->notes[i].pitch)});
          };
          if (selected_ >= 0)
            snapshot(selected_);
          for (const int i : selectSet_)
            if (i != selected_)
              snapshot(i);
        } else {
          selectSet_.clear();
        }
        owner_->audition(misuc::dsp::midiNoteToFreq(static_cast<double>(n.pitch)),
                         n.velocity / 127.0f);
      } else {
        selected_ = -1;
        selectSet_.clear();
        dragging_ = false;
        dragged_ = false;
        selecting_ = true;
        marquee0_ = marquee1_ = pos;
      }
      update();
    } else if (e->button() == Qt::RightButton) {
      const int hit = hitTest(pos);
      if (hit >= 0)
        deleteNote(hit);
      else {
        selected_ = -1;
        selectSet_.clear();
      }
      update();
    }
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    if (resizing_) {
      Track* t = trackEdit();
      if (t && resizeIndex_ >= 0 &&
          resizeIndex_ < static_cast<int>(t->notes.size())) {
        NoteEvent& n = t->notes[resizeIndex_];
        const double dur = std::clamp(
            snapped(xToBeat(e->position().x())) - n.startBeats,
            kMinDurationBeats, PianoRoll::kHorizonBeats - n.startBeats);
        if (std::fabs(dur - n.durationBeats) > 1e-6) {
          n.durationBeats = dur;
          resized_ = true;
          update();
        }
      }
      e->accept();
      return;
    }
    if (selecting_) {
      const Track* t = track();
      marquee1_ = e->position();
      QRectF box(marquee0_, marquee1_);
      box = box.normalized();
      selected_ = -1;
      selectSet_.clear();
      if (t) {
        for (size_t i = 0; i < t->notes.size(); ++i) {
          if (box.intersects(
                  noteRect(t->notes[i]).adjusted(0, 2, 0, -2))) {
            selected_ = static_cast<int>(i);
            selectSet_.push_back(static_cast<int>(i));
          }
        }
      }
      update();
      e->accept();
      return;
    }
    if (dragging_) {
      Track* t = trackEdit();
      if (dragIndex_ < 0 || dragIndex_ >= static_cast<int>(t->notes.size()))
        return;
      NoteEvent& n = t->notes[dragIndex_];
      const double beat = xToBeat(e->position().x());
      const int pitch = yToPitch(e->position().y());
      const double newStart =
          std::clamp(snapped(beat - dragOffsetBeats_), 0.0, PianoRoll::kHorizonBeats);
      const int newPitch = std::clamp(pitch - dragOffsetPitch_, kLowPitch, kHighPitch);
      if (multimove_) {
        const DragSnap* anchor = nullptr;
        for (const DragSnap& g : groupSnap_)
          if (g.index == dragIndex_)
            anchor = &g;
        if (!anchor)
          return;
        const double dStart = newStart - anchor->start;
        const int dPitch = newPitch - anchor->pitch;
        bool changed = false;
        for (const DragSnap& g : groupSnap_) {
          NoteEvent& gn = t->notes[g.index];
          const double s = std::clamp(g.start + dStart, 0.0,
                                      PianoRoll::kHorizonBeats);
          const int p = std::clamp(g.pitch + dPitch, kLowPitch, kHighPitch);
          if (std::fabs(s - gn.startBeats) > 1e-6 ||
              p != static_cast<int>(gn.pitch)) {
            gn.startBeats = s;
            gn.pitch = static_cast<uint8_t>(p);
            changed = true;
          }
        }
        if (changed) {
          dragged_ = true;
          owner_->audition(
              misuc::dsp::midiNoteToFreq(static_cast<double>(newPitch)),
              n.velocity / 127.0f);
        }
      } else {
        if (std::fabs(newStart - n.startBeats) > 1e-6 || newPitch != static_cast<int>(n.pitch)) {
          n.startBeats = newStart;
          n.pitch = static_cast<uint8_t>(newPitch);
          dragged_ = true;
          owner_->audition(
              misuc::dsp::midiNoteToFreq(static_cast<double>(n.pitch)),
              n.velocity / 127.0f);
        }
      }
      update();
      return;
    }
    if (onResizeGrip(e->position()))
      setCursor(Qt::SizeHorCursor);
    else
      unsetCursor();
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    Q_UNUSED(e);
    if (selecting_) {
      selecting_ = false;
      update();
      return;
    }
    if (resizing_) {
      resizing_ = false;
      if (resized_)
        owner_->commit();
      resized_ = false;
      return;
    }
    if (dragging_) {
      dragging_ = false;
      multimove_ = false;
      groupSnap_.clear();
      if (dragged_)
        owner_->commit();
      dragged_ = false;
    }
  }

  void mouseDoubleClickEvent(QMouseEvent* e) override {
    const QPointF pos = e->position();
    const int hit = hitTest(pos);
    if (hit >= 0)
      deleteNote(hit);
    else
      addNoteAt(xToBeat(pos.x()), yToPitch(pos.y()));
    owner_->commit();
  }

  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
      std::vector<int> doomed;
      if (selected_ >= 0)
        doomed.push_back(selected_);
      for (const int i : selectSet_)
        if (i != selected_)
          doomed.push_back(i);
      if (!doomed.empty()) {
        deleteNotes(doomed);
        owner_->commit();
        e->accept();
        return;
      }
    }
    QWidget::keyPressEvent(e);
  }

  void wheelEvent(QWheelEvent* e) override {
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
      const double factor = e->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
      owner_->setZoom(owner_->pxPerBeat_ * factor);
      e->accept();
      return;
    }
    // Wheel over an existing note changes its velocity (audible immediately,
    // committed once the gesture settles); wheel elsewhere scrolls.
    if (!e->modifiers().testFlag(Qt::AltModifier)) {
      const int hit = hitTest(e->position());
      if (hit >= 0) {
        Track* t = trackEdit();
        if (t && hit >= 0 && hit < static_cast<int>(t->notes.size())) {
          NoteEvent& n = t->notes[hit];
          const int delta = e->angleDelta().y();
          if (delta != 0) {
            const int step = delta > 0 ? kVelocityStep : -kVelocityStep;
            n.velocity = static_cast<uint8_t>(std::clamp(
                static_cast<int>(n.velocity) + step, 1, 127));
            selected_ = hit;
            selectSet_.clear();
            owner_->audition(
                misuc::dsp::midiNoteToFreq(static_cast<double>(n.pitch)),
                n.velocity / 127.0f);
            velocityTimer_.start(kVelocityCommitMs);
            update();
          }
          e->accept();
          return;
        }
      }
    }
    QWidget::wheelEvent(e);
  }

private:
  PianoRoll* owner_;
};

Track* PianoRoll::Canvas::trackEdit() {
  return owner_->session_.findTrack(owner_->trackId_.toStdString());
}

// --- PianoRoll -----------------------------------------------------------

PianoRoll::PianoRoll(misuc::project::Session& session, QWidget* parent)
    : QWidget(parent), session_(session) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto* topRow = new QHBoxLayout();
  topRow->setSpacing(0);

  auto* corner = new QWidget(this);
  corner->setFixedSize(kKeyWidth, kRulerH);
  corner->setStyleSheet(
      QStringLiteral("background-color: %1; border-bottom: 1px solid %2;")
          .arg(theme::kPanel.name(), theme::kBorder.name()));
  ruler_ = new PianoRuler(this);
  topRow->addWidget(corner);
  topRow->addWidget(ruler_, 1);
  root->addLayout(topRow);

  auto* lowRow = new QHBoxLayout();
  lowRow->setSpacing(0);
  keys_ = new PianoKeys(this);
  canvas_ = new Canvas(this);
  scroller_ = new QScrollArea(this);
  scroller_->setWidget(canvas_);
  scroller_->setWidgetResizable(false);
  scroller_->setFrameShape(QFrame::NoFrame);
  scroller_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroller_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  lowRow->addWidget(keys_);
  lowRow->addWidget(scroller_, 1);
  root->addLayout(lowRow, 1);

  hbar_ = scroller_->horizontalScrollBar();
  vbar_ = scroller_->verticalScrollBar();
  connect(hbar_, &QScrollBar::valueChanged, this, [this] {
    hbarValue_ = hbar_->value();
    refresh();
  });
  connect(vbar_, &QScrollBar::valueChanged, this, [this] {
    vbarValue_ = vbar_->value();
    keys_->update();
  });
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PianoRoll::setTrack(const QString& id) {
  trackId_ = id;
  canvas_->selected_ = -1;
  refresh();
}

void PianoRoll::setPlayhead(bool playing, double beats) {
  playheadPlaying_ = playing;
  playheadBeats_ = beats;
  refresh();
  autoFollow(playing, beats);
}

void PianoRoll::refresh() {
  const int barBeats = session_.timeSigNumerator();
  ruler_->setHorizon(kHorizonBeats, static_cast<int>(pxPerBeat_), barBeats,
                     hbarValue_);
  ruler_->setPlayheadBeat(playheadBeats_, playheadBeats_ >= 0.0);
  keys_->setRange(kLowPitch, kRowH, vbarValue_);
  if (canvas_) {
    canvas_->setPlayheadBeat(playheadBeats_, playheadBeats_ >= 0.0);
    canvas_->update();
  }
}

void PianoRoll::autoFollow(bool playing, double beats) {
  if (!canvas_ || !scroller_ || beats < 0.0)
    return;
  const int viewX = scroller_->viewport()->width();
  const int docW = canvas_->width();
  if (docW <= 0)
    return;
  const double cx = beats * pxPerBeat_;
  const int cur = hbar_->value();
  if (playing) {
    // Keep the caret inside the visible window while the transport runs.
    if (cx < cur || cx > cur + viewX)
      hbar_->setValue(std::clamp(static_cast<int>(cx - viewX * 0.1), 0,
                                 std::max(0, docW - viewX)));
  } else if (beats >= 0.0 && !playing) {
    // Manual scrub: bring the caret into view if it is off-screen.
    if (cx < cur || cx > cur + viewX)
      hbar_->setValue(std::clamp(static_cast<int>(cx), 0,
                                 std::max(0, docW - viewX)));
  }
}

void PianoRoll::setZoom(double px) {
  pxPerBeat_ = std::clamp(px, 8.0, 160.0);
  canvas_->setFixedSize(
      static_cast<int>(kHorizonBeats * pxPerBeat_) + 1,
      kRowCount * kRowH + 1);
  refresh();
}

QSize PianoRoll::sizeHint() const {
  return QSize(kKeyWidth + 640, kRulerH + 420);
}

void PianoRoll::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::kBackground);
}

} // namespace misuc::ui