#include "PianoRoll.h"

#include <algorithm>
#include <cmath>

#include <QFontMetrics>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <map>

#include "Theme.h"

#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/dsp/Midi.h>
#include <misuc/engine/dsp/Pitch.h>

using misuc::project::NoteEvent;
using misuc::project::AudioClip;
using misuc::project::Track;

namespace misuc::ui {

namespace {

constexpr double kSnapBeats = 0.03125; // finest selectable grid: 1/32
constexpr int kHighPitch = PianoRoll::kLowPitch + PianoRoll::kRowCount - 1;
constexpr int kVelocityStep = 127 / 25; // one wheel notch
constexpr int kVelocityCommitMs = 280;  // undo-snapshot debounce
constexpr double kResizeGrabPx = 6.0;   // right-edge grab band, px
constexpr double kMinDurationBeats = 0.1;
constexpr double kMinClipDurationBeats = 0.25; // clip min length on the timeline
constexpr float kClipGainStepDb = 1.0f;       // wheel step over a clip

int pitchRowY(int pitch, int rowH) {
  return PianoRoll::kClipLaneH + (kHighPitch - pitch) * rowH;
}

int yToPitch(int y, int rowH) {
  return std::clamp(kHighPitch - (y - PianoRoll::kClipLaneH) / rowH,
                    PianoRoll::kLowPitch, kHighPitch);
}

QString pitchLabel(int pitch) {
  return QString::fromStdString(misuc::dsp::midiToNoteName(pitch));
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

void PianoRuler::setLoop(bool on, double startBeats, double endBeats) {
  loopOn_ = on;
  loopStartBeats_ = startBeats;
  loopEndBeats_ = endBeats;
  update();
}

double PianoRuler::beatAtX(double x) const {
  return std::clamp((x + hbar_) / static_cast<double>(pxPerBeat_), 0.0,
                    numBeats_);
}

void PianoRuler::setRegionName(const QString& name) {
  regionName_ = name;
  update();
}

void PianoRuler::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    grabbingLoop_ = true;
    moved_ = false;
    grabStartBeat_ = grabEndBeat_ = beatAtX(event->position().x());
    update();
    return;
  }
  QWidget::mousePressEvent(event);
}

void PianoRuler::mouseMoveEvent(QMouseEvent* event) {
  if (grabbingLoop_) {
    moved_ = true;
    grabEndBeat_ = beatAtX(event->position().x());
    update();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void PianoRuler::mouseReleaseEvent(QMouseEvent* event) {
  if (grabbingLoop_) {
    grabbingLoop_ = false;
    const double lo = std::min(grabStartBeat_, grabEndBeat_);
    const double hi = std::max(grabStartBeat_, grabEndBeat_);
    if (moved_) {
      if (onLoopDrawn && hi - lo >= kSnapBeats)
        onLoopDrawn(lo, hi);
    } else if (onSeekClicked) {
      // A plain click (no drag) parks the playback caret on the ruler.
      onSeekClicked(beatAtX(event->position().x()));
    }
    update();
    return;
  }
  QWidget::mouseReleaseEvent(event);
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

  // Loop region: shaded area + edge lines (live drag preview when grabbing).
  if (loopOn_ || grabbingLoop_) {
    double lo = loopStartBeats_;
    double hi = loopEndBeats_;
    if (grabbingLoop_) {
      lo = std::min(grabStartBeat_, grabEndBeat_);
      hi = std::max(grabStartBeat_, grabEndBeat_);
    }
    if (hi > lo) {
      const double lx = lo * px - hbar_;
      const double hx = hi * px - hbar_;
      QRectF region(lx, 2, hx - lx, height() - 4);
      if (region.intersects(QRectF(-1, 2, width() + 2, height() - 4))) {
        p.fillRect(region, theme::kLoopRegion);
        p.setPen(QPen(theme::kOk, 1.5));
        p.drawLine(QPointF(lx, 2), QPointF(lx, height() - 2));
        p.drawLine(QPointF(hx, 2), QPointF(hx, height() - 2));
      }
    }
  }

  // Playhead caret aligned with the canvas line below.
  if (playheadVisible_ && playheadBeats_ >= 0.0) {
    const double cx = playheadBeats_ * px - hbar_;
    if (cx >= -1.0 && cx <= width() + 1.0) {
      p.setPen(QPen(theme::kPlayhead, 2));
      p.drawLine(QPointF(cx, 2), QPointF(cx, height() - 1));
    }
  }

  // Name of the region actually being edited, top-right.
  if (!regionName_.isEmpty()) {
    p.setPen(theme::kAccent);
    p.setFont(theme::monoFont(8));
    p.drawText(QRectF(0, 1, width() - 6, height() - 3),
               Qt::AlignVCenter | Qt::AlignRight, regionName_);
  }
}

// --- Keyboard ------------------------------------------------------------

PianoKeys::PianoKeys(QWidget* parent) : QWidget(parent) {
  setFixedWidth(PianoRoll::kKeyWidth);
}

QSize PianoKeys::sizeHint() const {
  return QSize(PianoRoll::kKeyWidth,
               PianoRoll::kRowCount * rowH_ +
                   PianoRoll::kClipLaneH + PianoRoll::kVelLaneH);
}

void PianoKeys::setRange(int lowPitch, int rowH, int vbar) {
  lowPitch_ = lowPitch;
  rowH_ = rowH;
  vbar_ = vbar;
  update();
}

int PianoKeys::pitchAtY(int y) const {
  return std::clamp(yToPitch(y, rowH_), lowPitch_, PianoRoll::kLowPitch +
                        PianoRoll::kRowCount - 1);
}

void PianoKeys::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    const int p = pitchAtY(static_cast<int>(event->position().y()));
    if (p >= 0)
      emit keyPressed(p);
    return;
  }
  QWidget::mousePressEvent(event);
}

void PianoKeys::mouseMoveEvent(QMouseEvent* event) {
  if (event->buttons() & Qt::LeftButton) {
    const int p = pitchAtY(static_cast<int>(event->position().y()));
    if (p >= 0)
      emit keyPressed(p); // glissando while dragging
    return;
  }
  QWidget::mouseMoveEvent(event);
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
    if (misuc::dsp::isBlackKey(pitch)) {
      p.fillRect(row, theme::kPanelSunken);
      p.setPen(QPen(theme::kBorder, 1));
      p.drawRect(row.adjusted(0, 0, -1, -1));
    } else {
      p.fillRect(row, theme::kGridRow);
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
    setObjectName(QStringLiteral("pianoCanvas")); // QA-harness navigation handle
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(
        static_cast<int>(owner_->horizonBeats() * owner_->pxPerBeat_) + 1,
        PianoRoll::kRowCount * owner_->rowH_ + PianoRoll::kClipLaneH +
            PianoRoll::kVelLaneH + 1);
    // Wheel velocity edits are committed once the gesture settles so a single
    // scroll burst produces exactly one undo snapshot.
    velocityTimer_.setSingleShot(true);
    connect(&velocityTimer_, &QTimer::timeout, this, [this] { owner_->commit(); });
    clipGainTimer_.setSingleShot(true);
    connect(&clipGainTimer_, &QTimer::timeout, this, [this] { owner_->commit(); });
  }

  QSize sizeHint() const override { return size(); }

  double xToBeat(double x) const { return x / owner_->pxPerBeat_; }
  double beatToX(double beat) const { return beat * owner_->pxPerBeat_; }
  int yToPitch(int y) const { return ::misuc::ui::yToPitch(y, owner_->rowH_); }
  int pitchToY(int pitch) const {
    return ::misuc::ui::pitchRowY(pitch, owner_->rowH_);
  }
  double snapped(double beat) const {
    if (!owner_->snap_)
      return beat;
    return std::round(beat / owner_->snapBeats_) * owner_->snapBeats_;
  }

  // Quantize always snaps to the fixed 1/16 grid, regardless of the snap
  // toggle (which only gates manual placement/drag gestures).
  double quantized(double beat) const {
    return std::round(beat / 0.25) * 0.25;
  }

  void setPlayheadBeat(double beats, bool visible) {
    playheadBeats_ = beats;
    playheadVisible_ = visible;
    update();
  }

  const Track* track() const { return owner_->session_.findTrack(owner_->trackId_.toStdString()); }
  Track* trackEdit();

  // Note storage actually being edited: the bound region's local notes, or the
  // track's legacy absolute notes when no region is bound (or it was removed).
  std::vector<NoteEvent>* notesRef() {
    Track* t = trackEdit();
    if (!t)
      return nullptr;
    for (auto& r : t->regions)
      if (QString::fromStdString(r.id) == owner_->regionId_)
        return &r.notes;
    return &t->notes;
  }
  const std::vector<NoteEvent>* notesRef() const {
    const Track* t = track();
    if (!t)
      return nullptr;
    for (const auto& r : t->regions)
      if (QString::fromStdString(r.id) == owner_->regionId_)
        return &r.notes;
    return &t->notes;
  }

  // True while editing a real region (the clip lane is hidden in this mode so
  // the whole editor shares the region's local time frame).
  bool regionBound() const { return !owner_->regionId_.isEmpty(); }

  double regionStartBeats() const {
    const Track* t = track();
    if (!t || !regionBound())
      return 0.0;
    for (const auto& r : t->regions)
      if (QString::fromStdString(r.id) == owner_->regionId_)
        return r.startBeats;
    return 0.0;
  }

  // Global transport beat -> local editor frame (region-relative when bound).
  double localBeat(double globalBeats) const {
    if (globalBeats < 0.0)
      return -1.0;
    return globalBeats - regionStartBeats();
  }

  int hitTest(const QPointF& pos) const {
    const Track* t = track();
    if (!t)
      return -1;
    for (size_t i = 0; i < (*notesRef()).size(); ++i) {
      if (noteRect((*notesRef())[i]).adjusted(0, 2, 0, -2).contains(pos))
        return static_cast<int>(i);
    }
    return -1;
  }

  // True when a press/click lands in the right-edge grab band of a note.
  bool onResizeGrip(const QPointF& pos) const {
    const Track* t = track();
    if (!t)
      return false;
    for (size_t i = 0; i < (*notesRef()).size(); ++i) {
      const QRectF r = noteRect((*notesRef())[i]);
      if (r.adjusted(0, 2, 0, -2).contains(pos) &&
          pos.x() >= r.right() - kResizeGrabPx)
        return true;
    }
    return false;
  }

  // Index of the note whose velocity bar contains x (pitches are irrelevant
  // inside the lane; the nearest bar wins when bars overlap).
  int hitTestVelocity(double x) const {
    const std::vector<NoteEvent>* nb = notesRef();
    if (!nb || nb->empty())
      return -1;
    int best = -1;
    double bestDist = 1e18;
    for (size_t i = 0; i < nb->size(); ++i) {
      const QRectF r = noteRect((*nb)[i]);
      const double bw = std::max(3.0, r.width() * 0.6);
      const double c = r.left() + (r.width() - bw) / 2.0 + bw / 2.0;
      if (bestDist > std::fabs(x - c)) {
        bestDist = std::fabs(x - c);
        best = static_cast<int>(i);
      }
    }
    return best;
  }

  uint8_t velocityFromY(double y) const {
    const double laneBottom = height() - 4.0;
    const double laneMax = laneBottom - (static_cast<double>(height()) -
                                         PianoRoll::kVelLaneH + 8.0);
    const double v =
        (laneBottom - y) / std::max(1.0, laneMax) * 127.0;
    return static_cast<uint8_t>(std::clamp(static_cast<int>(v), 1, 127));
  }

  QRectF noteRect(const NoteEvent& n) const {
    const double x = beatToX(n.startBeats);
    const double w = std::max<double>(3.0, n.durationBeats * owner_->pxPerBeat_ - 1.0);
    const int y = pitchToY(n.pitch);
    return QRectF(x, y, w, owner_->rowH_ - 1);
  }

  // --- Audio clips -------------------------------------------------------

  QRectF clipRect(const AudioClip& c) const {
    const double x = beatToX(c.startBeats);
    const double w = std::max<double>(3.0, c.durationBeats * owner_->pxPerBeat_ - 1.0);
    return QRectF(x, 3, w, PianoRoll::kClipLaneH - 6);
  }

  int hitTestClip(const QPointF& pos) const {
    const Track* t = track();
    if (!t || pos.y() >= PianoRoll::kClipLaneH)
      return -1;
    for (size_t i = 0; i < t->clips.size(); ++i)
      if (clipRect(t->clips[i]).contains(pos))
        return static_cast<int>(i);
    return -1;
  }

  // 0 = body, 1 = trim-in (left edge), 2 = resize (right edge).
  int clipEdgeGrip(const QPointF& pos) const {
    const Track* t = track();
    if (!t || pos.y() >= PianoRoll::kClipLaneH)
      return 0;
    for (size_t i = 0; i < t->clips.size(); ++i) {
      const QRectF r = clipRect(t->clips[i]).adjusted(0, 2, 0, -2);
      if (!r.contains(pos))
        continue;
      if (pos.x() >= r.right() - kResizeGrabPx)
        return 2;
      if (pos.x() <= r.left() + kResizeGrabPx)
        return 1;
    }
    return 0;
  }

  // Decimated per-beat peak bins (16/beat, capped at 1024) for the clip's
  // waveform. Cached by a content signature so the paint loop (30 Hz) never
  // re-reads audio files; a stale cache entry just triggers one re-read.
  struct WaveCache {
    std::vector<float> peaks;
    bool ok = false;
  };

  const WaveCache& waveFor(const AudioClip& c) {
    const size_t bins = std::clamp<size_t>(
        static_cast<size_t>(std::ceil(c.durationBeats * 16.0)), 1, 1024);
    QFileInfo fi(QString::fromStdString(c.sourceFile));
    const std::string sig =
        c.id + "#" + std::to_string(bins) + "#" +
        std::to_string(fi.size()) + "#" +
        QString::number(fi.lastModified().toMSecsSinceEpoch()).toStdString();
    auto it = waves_.find(sig);
    if (it != waves_.end())
      return it->second;
    WaveCache wc;
    misuc::audio::AudioBuffer b;
    std::string err;
    if (misuc::audio::readAudioFile(c.sourceFile, b, err)) {
      const uint32_t ch = std::max<uint32_t>(1, b.channels);
      const uint64_t nFrames = b.frames;
      wc.peaks.assign(bins, 0.f);
      for (size_t bin = 0; bin < bins; ++bin) {
        const uint64_t s0 = (nFrames * bin) / bins;
        const uint64_t s1 = (nFrames * (bin + 1)) / bins;
        float mx = 0.f;
        for (uint64_t k = s0; k < s1; ++k)
          for (uint32_t ci = 0; ci < ch; ++ci)
            mx = std::max(mx, std::fabs(b.samples[static_cast<size_t>(k) * ch + ci]));
        wc.peaks[bin] = mx;
      }
      wc.ok = true;
    }
    return waves_.emplace(sig, std::move(wc)).first->second;
  }

  void addNoteAt(double beat, int pitch) {
    owner_->ensureRegion(); // editing always lands in a real session region
    Track* t = trackEdit();
    if (!t)
      return;
    NoteEvent n;
    n.pitch = static_cast<uint8_t>(pitch);
    n.velocity = 100;
    n.startBeats = snapped(beat);
    n.durationBeats = 0.5;
    const int idx = static_cast<int>((*notesRef()).size());
    (*notesRef()).push_back(n);
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

  bool isClipSelected(int index) const { return index == selectedClip_; }

  // Removes the given (descending) indices and invalidates all selection
  // state; callers commit once afterward.
  void deleteNotes(std::vector<int> indices) {
    Track* t = trackEdit();
    if (!t)
      return;
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    for (const int i : indices) {
      if (i < 0 || i >= static_cast<int>((*notesRef()).size()))
        continue;
      (*notesRef()).erase((*notesRef()).begin() + i);
    }
    selected_ = -1;
    selectSet_.clear();
    update();
  }

  void deleteNote(int index) {
    deleteNotes({index});
    update();
  }

  // Duplicates the selected clip one snap step to the right of itself into a
  // fresh clip (new id), selecting the copy, in a single undo snapshot.
  void duplicateClip() {
    Track* t = trackEdit();
    if (!t || selectedClip_ < 0 ||
        selectedClip_ >= static_cast<int>(t->clips.size()))
      return;
    AudioClip copy = t->clips[selectedClip_];
    const double ns =
        std::clamp(copy.startBeats + copy.durationBeats +
                       owner_->snapBeats_,
                   0.0, owner_->horizonBeats() - copy.durationBeats);
    copy.id.clear(); // Session::addClip assigns a new unique id
    copy.startBeats = ns;
    owner_->session_.addClip(t->id, std::move(copy));
    selectedClip_ = static_cast<int>(t->clips.size()) - 1;
    owner_->commit();
    update();
  }

  struct ClipNote {
    double relStart;
    int pitch;
    int velocity;
    double dur;
  };

  // Snaps the selection's start beats to the grid (or every note when nothing
  // is selected) and commits one undo snapshot if anything moved.
  void quantizeSelection() {
    Track* t = trackEdit();
    if (!t)
      return;
    std::vector<int> targets;
    if (selected_ >= 0)
      targets.push_back(selected_);
    for (const int i : selectSet_)
      if (i != selected_)
        targets.push_back(i);
    if (targets.empty())
      for (size_t i = 0; i < (*notesRef()).size(); ++i)
        targets.push_back(static_cast<int>(i));
    bool changed = false;
    for (const int i : targets) {
      if (i < 0 || i >= static_cast<int>((*notesRef()).size()))
        continue;
      NoteEvent& n = (*notesRef())[i];
      const double q = quantized(n.startBeats);
      if (std::fabs(q - n.startBeats) > 1e-6) {
        n.startBeats = q;
        changed = true;
      }
    }
    update();
    if (changed)
      owner_->commit();
  }

  // Velocity tool: nudges the selection's velocities by `delta` (clamped to
  // 1..127) in one undo snapshot. Operates on the selection only, so the nudge
  // never surprises the user by touching notes they did not pick.
  void nudgeVelocity(int delta) {
    Track* t = trackEdit();
    if (!t)
      return;
    std::vector<int> targets;
    if (selected_ >= 0)
      targets.push_back(selected_);
    for (const int i : selectSet_)
      if (i != selected_)
        targets.push_back(i);
    if (targets.empty())
      return;
    bool changed = false;
    for (const int i : targets) {
      if (i < 0 || i >= static_cast<int>((*notesRef()).size()))
        continue;
      NoteEvent& n = (*notesRef())[i];
      const int v = std::clamp(static_cast<int>(n.velocity) + delta, 1, 127);
      if (v != static_cast<int>(n.velocity)) {
        n.velocity = static_cast<uint8_t>(v);
        changed = true;
      }
    }
    update();
    if (changed)
      owner_->commit();
  }

  void copySelection() {
    Track* t = trackEdit();
    if (!t)
      return;
    std::vector<int> all;
    if (selected_ >= 0)
      all.push_back(selected_);
    for (const int i : selectSet_)
      if (i != selected_)
        all.push_back(i);
    if (all.empty())
      return;
    std::sort(all.begin(), all.end());
    const double minStart = (*notesRef())[all.front()].startBeats;
    clipboard_.clear();
    for (const int i : all) {
      const NoteEvent& n = (*notesRef())[i];
      clipboard_.push_back(
          {n.startBeats - minStart, n.pitch, n.velocity, n.durationBeats});
    }
    clipAnchor_ = minStart;
  }

  // Inserts clipboard copies anchored at `base`, selects them, and commits
  // exactly one undo snapshot for the whole paste.
  void insertPasted(double base) {
    Track* t = trackEdit();
    if (!t || clipboard_.empty())
      return;
    std::vector<int> added;
    for (const ClipNote& c : clipboard_) {
      NoteEvent n;
      n.pitch = static_cast<uint8_t>(c.pitch);
      n.velocity = static_cast<uint8_t>(c.velocity);
      n.durationBeats = c.dur;
      n.startBeats =
          std::clamp(base + c.relStart, 0.0, owner_->horizonBeats());
      (*notesRef()).push_back(n);
      added.push_back(static_cast<int>((*notesRef()).size()) - 1);
    }
    selected_ = added.back();
    selectSet_.clear();
    for (const int i : added)
      if (i != selected_)
        selectSet_.push_back(i);
    update();
    owner_->commit();
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
  std::vector<ClipNote> clipboard_;
  double clipAnchor_ = -1.0;
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
  int velDragIndex_ = -1;
  bool velDragCommited_ = false;
  int selectedClip_ = -1;
  bool clipDragging_ = false;
  bool clipDragged_ = false;
  int clipDragIndex_ = -1;
  double clipDragOffsetBeats_ = 0.0;
  // Clip edge gestures: clipResizeMode_ is 1 (trim-in, left edge) or 2
  // (resize, right edge); anchors capture the pre-gesture geometry so both
  // modes are undoable from one snapshot.
  int clipResizeMode_ = 0;
  bool clipResizing_ = false;
  bool clipResized_ = false;
  int clipResizeIndex_ = -1;
  double clipResizeStart_ = 0.0;
  double clipResizeDur_ = 0.0;
  double clipResizeOffset_ = 0.0;
  std::map<std::string, WaveCache> waves_;
  double playheadBeats_ = -1.0;
  bool playheadVisible_ = false;
  QTimer velocityTimer_;
  QTimer clipGainTimer_;

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanelSunken);
    const Track* t = track();
    const int barBeats = owner_->session_.timeSigNumerator();
    const int lastBeat = static_cast<int>(owner_->horizonBeats());
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

    // Audio-clip lane (hidden while editing a region: the whole editor then
    // shares the region's local time frame, and clips belong to the
    // arrangement Playlist instead).
    const bool clipLaneActive = !regionBound();
    if (selectedClip_ >= static_cast<int>(t->clips.size()))
      selectedClip_ = -1;
    if (clipLaneActive) {
      p.setPen(QPen(theme::kBorder, 1));
      p.drawLine(QPointF(0, PianoRoll::kClipLaneH),
                 QPointF(width(), PianoRoll::kClipLaneH));
    }
    if (clipLaneActive) {
    for (size_t i = 0; i < t->clips.size(); ++i) {
      const AudioClip& c = t->clips[i];
      const QRectF r = clipRect(c);
      const bool sel = isClipSelected(static_cast<int>(i));
      p.setPen(QPen(sel ? theme::kOk : theme::kBorderBright, 1));
      p.setBrush(theme::kClipBody);
      p.drawRoundedRect(r.adjusted(0, 0, -1, -1), 3, 3);
      const WaveCache& w = waveFor(c);
      if (w.ok && w.peaks.size() > 1) {
        const double wTop = r.top() + 2;
        const double wBot = r.bottom() - 2;
        const double wH = wBot - wTop;
        const double binW = r.width() / static_cast<double>(w.peaks.size());
        for (size_t bin = 0; bin < w.peaks.size(); ++bin) {
          const double bx = r.left() + bin * binW;
          const float amp = std::clamp(w.peaks[bin], 0.f, 1.f);
          const double vh = std::max(1.0, amp * wH);
          p.fillRect(QRectF(bx, wBot - vh, std::max(1.0, binW), vh),
                     theme::withAlpha(theme::kClipWave, amp > 0.f ? 190 : 60));
        }
      }
      if (sel) {
        const double hx = 4.0;
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kOk);
        p.drawRect(QRectF(r.left() + 1.0, r.top() + 3.0, hx, r.height() - 6.0));
        p.drawRect(QRectF(r.right() - hx - 1.0, r.top() + 3.0, hx, r.height() - 6.0));
      }
      const double nw = r.width() - 8;
      if (nw > 24) {
        p.setPen(theme::kTextDim);
        p.setFont(theme::monoFont(8));
        const QFontMetrics fm = p.fontMetrics();
        p.drawText(
            r.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
            fm.elidedText(QString::fromStdString(c.name), Qt::ElideRight,
                          static_cast<int>(nw)));
      }
    }
    }

    for (size_t i = 0; i < (*notesRef()).size(); ++i) {
      const NoteEvent& n = (*notesRef())[i];
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
          (static_cast<double>(n.velocity) / 127.0) * (owner_->rowH_ - 8);
      p.setPen(Qt::NoPen);
      p.setBrush(theme::kOk);
      p.drawRect(
          QRectF(r.left() + 3.0, r.bottom() - 1.0 - vh, 3.0, vh));
      p.setBrush(fill);
    }

    // Velocity lane: a per-note bar strip under the grid. Drag inside it to
    // reshape a note's velocity; the bar height mirrors the note's velocity.
    const int laneTop = height() - PianoRoll::kVelLaneH;
    p.fillRect(QRectF(0, laneTop, width(), PianoRoll::kVelLaneH),
               theme::kPanelRaised);
    p.setPen(QPen(theme::kBorder, 1));
    p.drawLine(QPointF(0, laneTop), QPointF(width(), laneTop));
    const double laneBottom = height() - 4.0;
    const double laneMax = laneBottom - (laneTop + 8.0);
    for (size_t i = 0; i < (*notesRef()).size(); ++i) {
      const NoteEvent& n = (*notesRef())[i];
      const QRectF r = noteRect(n);
      const double vh = std::max(1.0,
          (static_cast<double>(n.velocity) / 127.0) * laneMax);
      const bool sel = isSelected(static_cast<int>(i));
      QColor c = sel ? theme::kOk
                     : theme::withAlpha(theme::kAccent, 170);
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      const double bw = std::max(3.0, r.width() * 0.6);
      p.drawRect(QRectF(r.left() + (r.width() - bw) / 2.0, laneBottom - vh,
                        bw, vh));
    }

    // Playhead caret, drawn above the notes so it stays visible.
    if (playheadVisible_ && playheadBeats_ >= 0.0) {
      const double cx = beatToX(localBeat(playheadBeats_));
      if (cx >= -1.0 && cx <= width() + 1.0) {
        p.setPen(QPen(theme::kPlayhead, 2));
        p.drawLine(QPointF(cx, 0), QPointF(cx, height() - PianoRoll::kVelLaneH));
      }
    }

    // Rubber-band selection marquee.
    if (selecting_) {
      QRectF box(marquee0_, marquee1_);
      box = box.normalized();
      p.setPen(QPen(theme::kMarquee, 1, Qt::DashLine));
      p.setBrush(theme::withAlpha(theme::kLoopRegion, 40));
      p.drawRect(box);
    }
  }

  void mousePressEvent(QMouseEvent* e) override {
    setFocus();
    const QPointF pos = e->position();
    if (e->button() == Qt::LeftButton) {
      // Velocity lane: a press here re-shapes the velocity of the note whose
      // bar sits under the cursor (or selects it when already at that spot).
      if (pos.y() >= height() - PianoRoll::kVelLaneH) {
        const int hit = hitTestVelocity(pos.x());
        if (hit >= 0) {
          NoteEvent& n = (*notesRef())[hit];
          selected_ = hit;
          selectSet_.clear();
          velDragIndex_ = hit;
          velDragCommited_ = false;
          n.velocity = velocityFromY(pos.y());
          owner_->audition(misuc::dsp::midiNoteToFreq(
                               static_cast<double>(n.pitch)),
                           n.velocity / 127.0f);
          update();
          e->accept();
          return;
        }
        e->accept();
        return;
      }
      const int chit = hitTestClip(pos);
      if (!regionBound() &&
          (chit >= 0 || (pos.y() < PianoRoll::kClipLaneH && track()))) {
        // Pressing in the clip lane operates on a clip (or clears clip
        // selection on empty lane space); it never starts note gestures.
        selectedClip_ = chit;
        selected_ = -1;
        selectSet_.clear();
        if (chit >= 0) {
          const Track* t = track();
          const int grip = clipEdgeGrip(pos);
          if (grip != 0) {
            // Edge gesture: trim-in (left) or resize (right).
            clipResizeMode_ = grip;
            clipResizing_ = true;
            clipResized_ = false;
            clipResizeIndex_ = chit;
            const AudioClip& c = t->clips[chit];
            clipResizeStart_ = c.startBeats;
            clipResizeDur_ = c.durationBeats;
            clipResizeOffset_ = c.clipOffsetBeats;
            clipDragging_ = false;
          } else {
            clipResizeMode_ = 0;
            clipResizing_ = false;
            clipDragIndex_ = chit;
            clipDragging_ = true;
            clipDragged_ = false;
            clipDragOffsetBeats_ =
                xToBeat(pos.x()) - t->clips[chit].startBeats;
          }
        } else {
          clipDragging_ = false;
          clipDragIndex_ = -1;
          clipResizing_ = false;
          clipResizeMode_ = 0;
        }
        update();
        return;
      }
      if (onResizeGrip(pos)) {
        const int hit = hitTest(pos);
        selected_ = hit;
        selectSet_.clear();
        resizeIndex_ = hit;
        resizing_ = true;
        resized_ = false;
        owner_->audition(misuc::dsp::midiNoteToFreq(
                             static_cast<double>((*notesRef())[hit].pitch)),
                         (*notesRef())[hit].velocity / 127.0f);
        update();
        return;
      }
      const int hit = hitTest(pos);
      if (hit >= 0) {
        const NoteEvent& n = (*notesRef())[hit];
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
                {i, (*notesRef())[i].startBeats, static_cast<int>((*notesRef())[i].pitch)});
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
      const int chit = hitTestClip(pos);
      if (chit >= 0) {
        Track* t = trackEdit();
        if (t && chit < static_cast<int>(t->clips.size())) {
          t->clips.erase(t->clips.begin() + chit);
          if (selectedClip_ >= chit)
            --selectedClip_;
          if (selectedClip_ < -1)
            selectedClip_ = -1;
          owner_->commit();
        }
        update();
        return;
      }
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
    if (velDragIndex_ >= 0) {
      Track* t = trackEdit();
      if (t && velDragIndex_ < static_cast<int>((*notesRef()).size())) {
        NoteEvent& n = (*notesRef())[velDragIndex_];
        const uint8_t v = velocityFromY(e->position().y());
        if (v != n.velocity) {
          n.velocity = v;
          velDragCommited_ = true;
          owner_->audition(misuc::dsp::midiNoteToFreq(
                               static_cast<double>(n.pitch)),
                           n.velocity / 127.0f);
          update();
        }
      }
      e->accept();
      return;
    }
    if (clipResizing_ && clipResizeMode_ != 0) {
      Track* t = trackEdit();
      if (t && clipResizeIndex_ >= 0 &&
          clipResizeIndex_ < static_cast<int>(t->clips.size())) {
        AudioClip& c = t->clips[clipResizeIndex_];
        const double cur = snapped(xToBeat(e->position().x()));
        if (clipResizeMode_ == 1) {
          // Trim-in: keep the right edge fixed and push the trim offset
          // deeper into the source as the left edge moves right.
          const double right = clipResizeStart_ + clipResizeDur_;
          const double maxStart = right - kMinClipDurationBeats;
          const double start =
              std::clamp(cur, 0.0, maxStart);
          const double dur = right - start;
          const double maxOffset = clipResizeOffset_ + clipResizeDur_ - dur;
          const double off = std::clamp(
              clipResizeOffset_ + (start - clipResizeStart_), 0.0,
              maxOffset < 0 ? 0.0 : maxOffset);
          if (std::fabs(start - c.startBeats) > 1e-6 ||
              std::fabs(off - c.clipOffsetBeats) > 1e-6) {
            c.startBeats = start;
            c.durationBeats = dur;
            c.clipOffsetBeats = off;
            clipResized_ = true;
            update();
          }
        } else {
          // Resize: keep the left edge and trim offset fixed, change length.
          double dur = cur - clipResizeStart_;
          dur = std::clamp(dur, kMinClipDurationBeats,
                           owner_->horizonBeats() - clipResizeStart_);
          if (std::fabs(dur - c.durationBeats) > 1e-6) {
            c.durationBeats = dur;
            clipResized_ = true;
            update();
          }
        }
      }
      e->accept();
      return;
    }
    if (clipDragging_) {
      Track* t = trackEdit();
      if (t && clipDragIndex_ >= 0 &&
          clipDragIndex_ < static_cast<int>(t->clips.size())) {
        const double beat = xToBeat(e->position().x());
        const double ns = std::clamp(
            snapped(beat - clipDragOffsetBeats_), 0.0,
            owner_->horizonBeats() - t->clips[clipDragIndex_].durationBeats);
        AudioClip& c = t->clips[clipDragIndex_];
        if (std::fabs(ns - c.startBeats) > 1e-6) {
          c.startBeats = ns;
          clipDragged_ = true;
          update();
        }
      }
      e->accept();
      return;
    }
    if (resizing_) {
      Track* t = trackEdit();
      if (t && resizeIndex_ >= 0 &&
          resizeIndex_ < static_cast<int>((*notesRef()).size())) {
        NoteEvent& n = (*notesRef())[resizeIndex_];
        const double dur = std::clamp(
            snapped(xToBeat(e->position().x())) - n.startBeats,
            kMinDurationBeats, owner_->horizonBeats() - n.startBeats);
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
        for (size_t i = 0; i < (*notesRef()).size(); ++i) {
          if (box.intersects(
                  noteRect((*notesRef())[i]).adjusted(0, 2, 0, -2))) {
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
      if (dragIndex_ < 0 || dragIndex_ >= static_cast<int>((*notesRef()).size()))
        return;
      NoteEvent& n = (*notesRef())[dragIndex_];
      const double beat = xToBeat(e->position().x());
      const int pitch = yToPitch(e->position().y());
      const double newStart =
          std::clamp(snapped(beat - dragOffsetBeats_), 0.0, owner_->horizonBeats());
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
          NoteEvent& gn = (*notesRef())[g.index];
          const double s = std::clamp(g.start + dStart, 0.0,
                                      owner_->horizonBeats());
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
    // Hover cursor: edge grip for notes, clip edge grip for the lane.
    if (!regionBound() && e->position().y() < PianoRoll::kClipLaneH && track()) {
      const int cgrip = clipEdgeGrip(e->position());
      setCursor(cgrip ? Qt::SizeHorCursor : Qt::ArrowCursor);
    } else if (onResizeGrip(e->position())) {
      setCursor(Qt::SizeHorCursor);
    } else {
      unsetCursor();
    }
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    Q_UNUSED(e);
    if (velDragIndex_ >= 0) {
      velDragIndex_ = -1;
      if (velDragCommited_) {
        owner_->commit();
        velDragCommited_ = false;
      }
      return;
    }
    if (clipResizing_) {
      clipResizing_ = false;
      clipResizeMode_ = 0;
      if (clipResized_)
        owner_->commit();
      clipResized_ = false;
      return;
    }
    if (clipDragging_) {
      clipDragging_ = false;
      clipDragIndex_ = -1;
      if (clipDragged_)
        owner_->commit();
      clipDragged_ = false;
      return;
    }
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
    if (!regionBound() && pos.y() < PianoRoll::kClipLaneH) {
      selectedClip_ = hitTestClip(pos);
      selected_ = -1;
      selectSet_.clear();
      update();
      return;
    }
    const int hit = hitTest(pos);
    if (hit >= 0)
      deleteNote(hit);
    else
      addNoteAt(xToBeat(pos.x()), yToPitch(pos.y()));
    owner_->commit();
  }

  void keyPressEvent(QKeyEvent* e) override {
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
      if (selectedClip_ >= 0) {
        Track* t = trackEdit();
        if (t && selectedClip_ < static_cast<int>(t->clips.size())) {
          t->clips.erase(t->clips.begin() + selectedClip_);
          selectedClip_ = -1;
          owner_->commit();
        }
        e->accept();
        return;
      }
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
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
      const int k = e->key();
      if (k == Qt::Key_C) {
        copySelection();
        e->accept();
        return;
      }
      if (k == Qt::Key_D) {
        if (selectedClip_ >= 0) {
          duplicateClip();
          e->accept();
          return;
        }
        copySelection();
        if (!clipboard_.empty())
          insertPasted(clipAnchor_ + owner_->snapBeats_);
        e->accept();
        return;
      }
      if (k == Qt::Key_V) {
        const double base =
            (playheadVisible_ && playheadBeats_ >= 0.0)
                ? snapped(playheadBeats_)
                : clipAnchor_ + owner_->snapBeats_;
        insertPasted(base);
        e->accept();
        return;
      }
    }
    if (e->key() == Qt::Key_Q) {
      quantizeSelection();
      e->accept();
      return;
    }
    if (e->modifiers().testFlag(Qt::AltModifier)) {
      const int k = e->key();
      if (k == Qt::Key_Up || k == Qt::Key_Down) {
        const int step = (e->modifiers().testFlag(Qt::ShiftModifier) ? 12 : 1);
        const int delta = (k == Qt::Key_Up) ? step : -step;
        nudgeVelocity(delta);
        e->accept();
        return;
      }
    }
    QWidget::keyPressEvent(e);
  }

  void wheelEvent(QWheelEvent* e) override {
    if (e->modifiers().testFlag(Qt::ControlModifier) &&
        e->modifiers().testFlag(Qt::ShiftModifier)) {
      // Ctrl+Shift+wheel = vertical zoom (row height per semitone).
      const int delta = e->angleDelta().y();
      if (delta != 0)
        owner_->setRowHeight(owner_->rowH_ + (delta > 0 ? 2 : -2));
      e->accept();
      return;
    }
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
      const double factor = e->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
      owner_->setZoom(owner_->pxPerBeat_ * factor);
      e->accept();
      return;
    }
    // Wheel over a selected clip on the lane changes its clip gain (dB),
    // committed once the gesture settles so one burst = one undo snapshot.
    if (!regionBound() && !e->modifiers().testFlag(Qt::AltModifier) &&
        e->position().y() < PianoRoll::kClipLaneH) {
      const int chit = hitTestClip(e->position());
      if (chit >= 0) {
        Track* t = trackEdit();
        if (t && chit < static_cast<int>(t->clips.size())) {
          const int delta = e->angleDelta().y();
          if (delta != 0) {
            AudioClip& c = t->clips[chit];
            selectedClip_ = chit;
            selected_ = -1;
            selectSet_.clear();
            c.gainDb = std::clamp(
                c.gainDb + (delta > 0 ? kClipGainStepDb : -kClipGainStepDb),
                -60.0f, 24.0f);
            clipGainTimer_.start(kVelocityCommitMs);
            update();
          }
          e->accept();
          return;
        }
      }
    }
    // Wheel over an existing note changes its velocity (audible immediately,
    // committed once the gesture settles); wheel elsewhere scrolls.
    if (!e->modifiers().testFlag(Qt::AltModifier)) {
      const int hit = hitTest(e->position());
      if (hit >= 0) {
        Track* t = trackEdit();
        if (t && hit >= 0 && hit < static_cast<int>((*notesRef()).size())) {
          NoteEvent& n = (*notesRef())[hit];
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
  setAttribute(Qt::WA_OpaquePaintEvent);
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
  ruler_->onLoopDrawn = [this](double lo, double hi) {
    const double sb = snap_ ? snapBeats_ : 0.25; // always grid-align loops
    const double fs = std::round(lo / sb) * sb;
    const double fe = std::round(hi / sb) * sb;
    if (fe - fs < sb)
      return;
    session_.setLoop(true, fs, fe);
    ruler_->setLoop(true, fs, fe);
    emit edited();
  };
  // A plain ruler click parks the transport caret at that beat (region-local
  // ruler position is translated back to the global timeline).
  ruler_->onSeekClicked = [this](double localBeat) {
    emit seekRequested(toGlobalBeat(localBeat));
  };

  auto* lowRow = new QHBoxLayout();
  lowRow->setSpacing(0);
  keys_ = new PianoKeys(this);
  // Clicking/dragging the key column auditions that pitch. Connected after
  // keys_ is constructed (a null sender would silently drop the connection).
  connect(keys_, &PianoKeys::keyPressed, this,
          [this](int pitch) {
            emit audition(misuc::dsp::midiNoteToFreq(
                              static_cast<double>(pitch)),
                          1.0f);
          });
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

void PianoRoll::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  if (centerMiddleOnce_ || !vbar_ || !scroller_ || !scroller_->viewport())
    return;
  centerMiddleOnce_ = true;
  // Land the first view on middle C (C4, MIDI 60) so the usable C1..C7
  // keyboard is immediately in view instead of the top of the 0..127 range.
  // Defer one event-loop turn so the scroll area has been laid out and its
  // range/maximum are final before we compute the offset.
  QMetaObject::invokeMethod(
      this,
      [this] {
        const double vh = scroller_->viewport()->height();
        const int target =
            static_cast<int>(kClipLaneH + (kHighPitch - 60.0) * rowH_ -
                             vh / 2.0);
        vbar_->setValue(target <= 0 ? 0 : std::min(target, vbar_->maximum()));
      },
      Qt::QueuedConnection);
}

void PianoRoll::setTrack(const QString& id) {
  trackId_ = id;
  regionId_.clear();
  const Track* t = session_.findTrack(id.toStdString());
  if (t && !t->regions.empty())
    regionId_ = QString::fromStdString(t->regions.front().id);
  canvas_->selected_ = -1;
  canvas_->selectedClip_ = -1;
  refresh();
}

void PianoRoll::setRegion(const QString& trackId, const QString& regionId) {
  trackId_ = trackId;
  const Track* t = session_.findTrack(trackId.toStdString());
  if (!t) {
    regionId_.clear();
  } else if (regionId.isEmpty()) {
    regionId_ = t->regions.empty()
                    ? QString()
                    : QString::fromStdString(t->regions.front().id);
  } else {
    // Bind to the requested region if it still exists; fall back to the first
    // region (or legacy notes) when it was deleted externally.
    bool found = false;
    for (const auto& r : t->regions)
      if (QString::fromStdString(r.id) == regionId)
        found = true;
    regionId_ = found ? regionId
                      : (t->regions.empty()
                             ? QString()
                             : QString::fromStdString(t->regions.front().id));
  }
  canvas_->selected_ = -1;
  canvas_->selectedClip_ = -1;
  refresh();
}

QString PianoRoll::regionName() const {
  const auto* r = session_.findRegion(trackId_.toStdString(),
                                      regionId_.toStdString());
  return r ? QString::fromStdString(r->name) : QString();
}

double PianoRoll::regionStart() const {
  const auto* r = session_.findRegion(trackId_.toStdString(),
                                      regionId_.toStdString());
  return r ? r->startBeats : 0.0;
}

double PianoRoll::localBeat(double globalBeat) const {
  if (globalBeat < 0.0)
    return -1.0;
  return globalBeat - regionStart();
}

double PianoRoll::toGlobalBeat(double local) const {
  if (local < 0.0)
    return -1.0;
  return local + regionStart();
}

void PianoRoll::ensureRegion() {
  Track* t = session_.findTrack(trackId_.toStdString());
  if (!t)
    return;
  if (!regionId_.isEmpty()) {
    if (session_.findRegion(trackId_.toStdString(), regionId_.toStdString()))
      return;
    regionId_.clear();
  }
  if (t->regions.empty())
    regionId_ = QString::fromStdString(
        session_.addRegion(trackId_.toStdString(), "MIDI 1", 0.0, 4.0));
  else
    regionId_ = QString::fromStdString(t->regions.front().id);
  refresh();
}

void PianoRoll::setLoop(bool on, double startBeats, double endBeats) {
  if (ruler_)
    ruler_->setLoop(on, startBeats, endBeats);
}

void PianoRoll::quantize() {
  canvas_->quantizeSelection();
}

void PianoRoll::nudgeVelocity(int delta) {
  canvas_->nudgeVelocity(delta);
}

void PianoRoll::setPlayhead(bool playing, double beats) {
  // Skip the full-edit refresh when nothing visible moved (MainWindow feeds
  // this at 30 Hz even while idle). While playing, guard against re-rendering
  // when the caret is off-screen (follow disabled): only the caret redraws.
  if (playheadPlaying_ == playing && std::abs(beats - playheadBeats_) < 1e-9)
    return;
  playheadPlaying_ = playing;
  playheadBeats_ = beats;
  refresh();
  autoFollow(playing, beats);
}

void PianoRoll::refresh() {
  const auto* r = session_.findRegion(trackId_.toStdString(),
                                      regionId_.toStdString());
  horizonBeats_ = r ? std::max(8.0, r->durationBeats + 4.0)
                    : kHorizonBeats;
  const int barBeats = session_.timeSigNumerator();
  ruler_->setHorizon(horizonBeats(), static_cast<int>(pxPerBeat_), barBeats,
                     hbarValue_);
  ruler_->setPlayheadBeat(localBeat(playheadBeats_), playheadBeats_ >= 0.0);
  ruler_->setLoop(session_.loopMode(), session_.loopStart(),
                  session_.loopEnd());
  ruler_->setRegionName(r ? QString::fromStdString(r->name) : QString());
  keys_->setRange(kLowPitch, rowH_, vbarValue_);
  if (canvas_) {
    canvas_->setPlayheadBeat(playheadBeats_, playheadBeats_ >= 0.0);
    const int w = static_cast<int>(horizonBeats() * pxPerBeat_) + 1;
    const int h = kRowCount * rowH_ + kClipLaneH + kVelLaneH + 1;
    if (canvas_->width() != w || canvas_->height() != h)
      canvas_->setFixedSize(w, h);
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
  const double cx = localBeat(beats) * pxPerBeat_;
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
      static_cast<int>(horizonBeats() * pxPerBeat_) + 1,
      kRowCount * rowH_ + kClipLaneH + kVelLaneH + 1);
  refresh();
}

void PianoRoll::setRowHeight(int rowH) {
  rowH_ = std::clamp(rowH, 12, 48);
  canvas_->setFixedSize(
      static_cast<int>(horizonBeats() * pxPerBeat_) + 1,
      kRowCount * rowH_ + kClipLaneH + kVelLaneH + 1);
  keys_->setRange(kLowPitch, rowH_, vbarValue_);
  keys_->update();
  refresh();
}

QSize PianoRoll::sizeHint() const {
  return QSize(kKeyWidth + 640, kRulerH + 420 + kVelLaneH);
}

void PianoRoll::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::kBackground);
}

} // namespace misuc::ui