#include "Playlist.h"

#include <algorithm>
#include <cmath>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QMenu>
#include <QInputDialog>
#include <QApplication>
#include <QTimer>
#include <QToolButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include "HintSystem.h"
#include "Theme.h"
#include <QFileDialog>
#include <QMimeData>
#include <QLabel>
#include <QKeyEvent>
#include <sndfile.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/project/Session.h>
#include "theme/ThemeEngine.h"
#include "Theme.h"
#include "branding/Icons.h"

namespace misuc::ui {

// ───────────────────────── constants ─────────────────────────
static constexpr int kRulerH = 24;
static constexpr int kLabelW = 168;
static constexpr int kRowH  = 52;
static constexpr int kSnapDiv = 4;      // snap to 1/4 beat (16th)
static constexpr float kPlayheadAlpha = 180;
static constexpr double kMinDur = 0.1;   // minimum clip duration (beats)
static constexpr int kResizeHit = 6;

static inline double quant(double b, int div = kSnapDiv) {
  if (div <= 0) return b;
  return std::round(b * div) / double(div);
}
static inline double clamp0(double v) { return v < 0 ? 0 : v; }

// ───────────────────────── Peaks cache ─────────────────────────
struct ClipPeaks {
  std::vector<float> mins, maxs;
};

static QHash<QString, ClipPeaks> s_peaksCache;

static const ClipPeaks* loadPeaks(const QString& path) {
  if (path.isEmpty()) return nullptr;
  auto it = s_peaksCache.constFind(path);
  if (it != s_peaksCache.constEnd()) return &it.value();

  SF_INFO info{};
  SNDFILE* sndfile = sf_open(path.toUtf8().constData(), SFM_READ, &info);
  if (!sndfile) return nullptr;

  const int numBuckets = 256;
  ClipPeaks peaks;
  peaks.mins.resize(numBuckets, 0.f);
  peaks.maxs.resize(numBuckets, 0.f);

  std::vector<float> buf(info.channels * 1024);
  for (int b = 0; b < numBuckets; ++b) {
    sf_seek(sndfile, (b * info.frames) / numBuckets, SEEK_SET);
    sf_count_t toRead = std::min<sf_count_t>(256, info.frames);
    sf_count_t read = sf_readf_float(sndfile, buf.data(), toRead);
    float lo = 0.f, hi = 0.f;
    for (sf_count_t f = 0; f < read; ++f)
      for (int c = 0; c < info.channels; ++c) {
        float v = buf[f * info.channels + c];
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
    peaks.mins[b] = lo;
    peaks.maxs[b] = hi;
  }
  sf_close(sndfile);
  s_peaksCache.insert(path, peaks);
  return &s_peaksCache[path];
}

// ───────────────────────── helpers ─────────────────────────
static QString clipName(const misuc::project::AudioClip& c) {
  if (c.name.empty()) {
    auto sp = QString::fromStdString(c.sourceFile).section(QLatin1Char('/'), -1);
    return sp.isEmpty() ? QStringLiteral("clip") : sp;
  }
  return QString::fromStdString(c.name);
}
static bool isMidi(const misuc::project::AudioClip& c) {
  return c.sourceFile.empty();
}
static QColor clipColor(const misuc::project::AudioClip& c, const QString& tokenId) {
  auto& te = ThemeEngine::instance();
  if (tokenId == "midi") return theme::kClipMidi;
  return theme::kClipBody;   // audio
}

// ───────────────────── PlaylistGrid ─────────────────────────
class PlaylistGrid : public QWidget {
public:
  misuc::project::Session& s_;
  Playlist* playlist_;
  bool playing_ = false;
  double playhead_ = 0;
  double pxPerBeat_ = 24;
  bool snapOn_ = false;
  bool songMode_ = true;

  int selRow_ = -1;
  int selClip_ = -1;
  bool selMidi_ = false;

  // drag state
  struct Drag { enum Kind {None, Move, Resize, Slice, Marquee, Scroll}; Kind kind=None; int row=-1; int clipIdx=-1; bool midi=false; double origStart=0; double origDur=0; QPoint pressPt, lastPt; double beat0=0; bool right=false; };
  Drag drag_;

  QHash<QString, QPixmap> waveformPixCache_;

  PlaylistGrid(misuc::project::Session& session, Playlist* playlist, QWidget* parent=nullptr)
    : QWidget(parent), s_(session), playlist_(playlist) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(kLabelW + 200);
  }

  QSize sizeHint() const override {
    int rows = qMax(1, int(s_.tracks().size()));
    double maxEnd = 32;
    for (auto& t : s_.tracks())
      for (auto& c : t.clips) maxEnd = qMax(maxEnd, c.startBeats + c.durationBeats);
    int w = int(kLabelW + (maxEnd + 4) * pxPerBeat_);
    return QSize(qMax(w, 600), rows * kRowH + kRulerH);
  }

  QString trackIdAt(int row) const {
    auto tracks = s_.tracks();
    if (row < 0 || row >= (int)tracks.size()) return {};
    return QString::fromStdString(tracks[row].id);
  }
  int trackRow(const QString& id) const {
    for (int i = 0; i < (int)s_.tracks().size(); ++i)
      if (QString::fromStdString(s_.tracks()[i].id) == id) return i;
    return -1;
  }

  // MIDI region: derived from track notes (no schema change)
  struct MidiRegion { double start, end; int count; };
  MidiRegion midiRegion(const misuc::project::Track& t) const {
    MidiRegion r{99999, 0, 0};
    for (auto& n : t.notes) {
      r.start = qMin(r.start, n.startBeats);
      r.end   = qMax(r.end,   n.startBeats + n.durationBeats);
      ++r.count;
    }
    if (r.count == 0) r = {-1, -1, 0};
    return r;
  }

  // ── hit test ──
  struct Hit { enum Kind {None, Track, Clip, ResizeRight, Mute, Solo}; Kind kind=None; int row=-1; int clipIdx=-1; bool midi=false; };
  Hit hitTest(QPoint pt) const {
    Hit h;
    int rows = qMax(1, int(s_.tracks().size()));
    int contentH = rows * kRowH + kRulerH;
    int contentW = int(kLabelW + (64) * pxPerBeat_);
    if (pt.y() < kRulerH || pt.x() < 0 || pt.y() > contentH || pt.x() > contentW) return h;

    int row = (pt.y() - kRulerH) / kRowH;
    if (row < 0 || row >= (int)s_.tracks().size()) {
      h.kind = Hit::None;
      return h;
    }
    h.row = row;
    if (pt.x() < kLabelW) {
      // label region: check mute/solo button
      int iconX = kLabelW - 42;
      int iconY = kRulerH + row * kRowH + (kRowH - 18) / 2;
      if (pt.x() >= iconX && pt.x() <= iconX + 18 && pt.y() >= iconY && pt.y() <= iconY + 18) {
        h.kind = Hit::Mute; return h;
      }
      iconX += 22;
      if (pt.x() >= iconX && pt.x() <= iconX + 18 && pt.y() >= iconY && pt.y() <= iconY + 18) {
        h.kind = Hit::Solo; return h;
      }
      h.kind = Hit::Track; return h;
    }

    // clip lane
    auto& t = s_.tracks()[row];
    for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
      auto& c = t.clips[ci];
      double x0 = kLabelW + c.startBeats * pxPerBeat_;
      double x1 = kLabelW + (c.startBeats + c.durationBeats) * pxPerBeat_;
      if (pt.x() >= x0 && pt.x() <= x1) {
        h.clipIdx = ci; h.midi = false;
        if (pt.x() >= x1 - kResizeHit) h.kind = Hit::ResizeRight;
        else h.kind = Hit::Clip;
        return h;
      }
    }
    // check MIDI region
    auto mr = midiRegion(t);
    if (mr.count > 0) {
      double mx0 = kLabelW + mr.start * pxPerBeat_;
      double mx1 = kLabelW + mr.end * pxPerBeat_;
      if (pt.x() >= mx0 && pt.x() <= mx1) {
        h.clipIdx = -1; h.midi = true;
        h.kind = Hit::Clip;
        return h;
      }
    }
    return h;
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    auto& te = ThemeEngine::instance();
    int W = width(), H = height();
    int rows = qMax(1, int(s_.tracks().size()));

    // ── bg
    p.fillRect(rect(), theme::kBackground);

    // ── ruler bg
    p.fillRect(0, 0, W, kRulerH, theme::kPanelHeader);
    p.setPen(theme::kBorderStrong);
    p.drawLine(0, kRulerH - 1, W, kRulerH - 1);

    // ruler ticks
    int beatW = int(pxPerBeat_);
    for (double b = 0; b < (W - kLabelW) / pxPerBeat_ + 1; b += 1) {
      int x = kLabelW + int(b * pxPerBeat_);
      bool bar = (int(b) % 4 == 0);
      int th = bar ? 10 : 4;
      p.setPen(bar ? theme::kGridStrong : theme::kGridRow);
      p.drawLine(x, kRulerH - th, x, kRulerH - 1);
      if (bar) {
        p.setPen(theme::kTextFaint);
        p.drawText(x + 3, 14, QString::number(int(b) / 4 + 1));
      }
    }

    // ── row lanes
    for (int r = 0; r < rows; ++r) {
      int y = kRulerH + r * kRowH;
      // lane bg alternating
      QColor laneBg = (r % 2 == 0) ? theme::kMatteSurface : theme::kBackground;
      if (r == selRow_) {
        QColor sel = laneBg;
        sel.setAlpha(60);
        laneBg = sel;
      }
      p.fillRect(kLabelW, y, W - kLabelW, kRowH, laneBg);

      // divider
      p.setPen(theme::kGridRow);
      p.drawLine(kLabelW, y + kRowH - 1, W, y + kRowH - 1);

      // ── label
      auto& t = s_.tracks()[r];
      QString name = QString::fromStdString(t.name);
      p.setPen(theme::kText);
      QFont f = p.font(); f.setPixelSize(13); p.setFont(f);
      p.drawText(10, y + kRowH / 2 + 5, name.left(16));

      // mute/solo icons
      int iconY = y + (kRowH - 18) / 2;
      int mx = kLabelW - 42;
      bool muted = t.muted, solo = t.solo;
      p.setPen(Qt::NoPen);
      p.setBrush(muted ? theme::kAccent : theme::kBorderStrong);
      p.drawEllipse(mx, iconY, 18, 18);
      if (muted) {
        p.setPen(QPen(theme::kText, 2));
        p.drawLine(mx + 4, iconY + 9, mx + 14, iconY + 9);
      }
      int sx = mx + 22;
      p.setPen(Qt::NoPen);
      p.setBrush(solo ? QColor(255,180,60) : theme::kBorderStrong);
      p.drawEllipse(sx, iconY, 18, 18);
      if (solo) {
        p.setPen(QPen(QColor(255,255,255), 2));
        QFont sf = p.font(); sf.setBold(true); sf.setPixelSize(10); p.setFont(sf);
        p.drawText(QRect(sx, iconY, 18, 18), Qt::AlignCenter, "S");
        p.setFont(f);
      }

      // ── audio clips
      for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
        auto& c = t.clips[ci];
        double x0 = kLabelW + c.startBeats * pxPerBeat_;
        double w  = c.durationBeats * pxPerBeat_;
        double x1 = x0 + w;
        if (x1 < kLabelW || x0 > W) continue;
        QRectF body(x0, y + 4, w, kRowH - 8);
        bool sel = (r == selRow_ && ci == selClip_ && !selMidi_);
        QColor bg = isMidi(c) ? theme::kClipMidi : theme::kClipBody;
        if (c.gainDb < -90.f) bg.setAlpha(120);
        p.setPen(sel ? theme::kAccent : theme::kGridStrong);
        p.setBrush(bg);
        p.drawRoundedRect(body, 4, 4);

        // name
        p.setPen(theme::kText);
        QFont cf = p.font(); cf.setPixelSize(11); p.setFont(cf);
        p.drawText(body.adjusted(6,2,-6,-2), Qt::TextSingleLine, clipName(c));

        // waveform for audio
        if (!isMidi(c) && c.sourceFile.size()) {
          auto* pk = loadPeaks(QString::fromStdString(c.sourceFile));
          if (pk) {
            int n = pk->mins.size();
            double midY = body.center().y();
            double halfH = body.height() * 0.32;
            QPolygonF waveMin, waveMax;
            for (int i = 0; i < n; ++i) {
              double xp = body.left() + body.width() * i / double(n);
              waveMin << QPointF(xp, midY + pk->mins[i] * halfH);
              waveMax << QPointF(xp, midY + pk->maxs[i] * halfH);
            }
            p.setPen(Qt::NoPen);
            QColor waveCol = theme::kText;
            waveCol.setAlpha(90);
            p.setBrush(waveCol);
            QPainterPath wavePath;
            wavePath.addPolygon(waveMax);
            for (int i = n - 1; i >= 0; --i)
              wavePath.lineTo(waveMin[i]);
            wavePath.closeSubpath();
            p.drawPath(wavePath);
          }
        }
      }

      // ── MIDI region
      auto mr = midiRegion(t);
      if (mr.count > 0) {
        double mx0 = kLabelW + mr.start * pxPerBeat_;
        double mw  = (mr.end - mr.start) * pxPerBeat_;
        if (mx0 + mw > kLabelW && mx0 < W) {
          QRectF mbody(mx0, y + 4, mw, kRowH - 8);
          bool selM = (r == selRow_ && selMidi_);
          p.setPen(selM ? theme::kAccent : theme::kGridStrong);
          QColor mBg = theme::kClipMidi;
          p.setBrush(mBg);
          p.drawRoundedRect(mbody, 4, 4);

          // mini note bars inside
          int noteH = 4;
          p.setPen(Qt::NoPen);
          QColor noteCol = theme::kText; noteCol.setAlpha(70);
          p.setBrush(noteCol);
          int pitches[8] = {60,62,64,65,67,69,71,72};
          for (auto& n : t.notes) {
            double nx = mbody.left() + (n.startBeats - mr.start) * pxPerBeat_;
            double nw = n.durationBeats * pxPerBeat_;
            int pitchIdx = qBound(0, n.pitch % 8, 7);
            double ny = mbody.top() + (pitchIdx / 8.0) * (mbody.height() - noteH);
            p.drawRect(nx, ny, qMax(nw, 2.0), noteH);
          }
          p.setPen(theme::kText);
          QFont mf = p.font(); mf.setPixelSize(10); p.setFont(mf);
          p.drawText(mbody.adjusted(6,1,-6,-1), Qt::TextSingleLine,
                     QString("MIDI · %1 notes").arg(mr.count));
        }
      }
    }

    // ── playhead
    if (playhead_ >= 0) {
      int px = kLabelW + int(playhead_ * pxPerBeat_);
      QColor ph = theme::kPlayhead;
      p.setPen(QPen(ph, 2));
      p.drawLine(px, kRulerH, px, H);
      // triangle in ruler
      QPolygonF tri; tri << QPointF(px - 5, 0) << QPointF(px + 5, 0) << QPointF(px, 9);
      p.setPen(Qt::NoPen); p.setBrush(ph);
      p.drawPolygon(tri);
    }

    // ── marquee
    if (drag_.kind == Drag::Marquee) {
      QRectF box = QRectF(drag_.pressPt, drag_.lastPt).normalized();
      QColor m = theme::kSelection; m.setAlpha(40);
      p.setBrush(m);
      p.setPen(QPen(theme::kAccent, 1, Qt::DashLine));
      p.drawRect(box);
    }
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    drag_.pressPt = e->pos();
    drag_.lastPt  = e->pos();
    Hit h = hitTest(e->pos());
    drag_.row    = h.row;
    drag_.clipIdx = h.clipIdx;
    drag_.midi   = h.midi;

    if (h.kind == Hit::Mute && h.row >= 0) {
      auto& t = s_.tracks()[h.row];
      t.muted = !t.muted;
      emit playlist_->edited();
      update();
      return;
    }
    if (h.kind == Hit::Solo && h.row >= 0) {
      auto& t = s_.tracks()[h.row];
      t.solo = !t.solo;
      emit playlist_->edited();
      update();
      return;
    }
    if (h.kind == Hit::Track && h.row >= 0) {
      selRow_ = h.row; selClip_ = -1; selMidi_ = false;
      emit playlist_->trackSelected(trackIdAt(selRow_));
      emit playlist_->selectionChanged();
      update();
      return;
    }
    if (h.kind == Hit::Clip && h.row >= 0) {
      selRow_ = h.row; selClip_ = h.clipIdx; selMidi_ = h.midi;
      emit playlist_->trackSelected(trackIdAt(selRow_));
      emit playlist_->selectionChanged();
      auto& t = s_.tracks()[selRow_];
      double start = h.midi ? -1 : t.clips[h.clipIdx].startBeats;
      drag_.origStart = start;
      drag_.origDur   = h.midi ? -1 : t.clips[h.clipIdx].durationBeats;
      drag_.kind = Drag::Move;
      drag_.beat0 = double(e->pos().x() - kLabelW) / pxPerBeat_;
      update();
      return;
    }
    if (h.kind == Hit::ResizeRight && h.row >= 0 && !h.midi) {
      selRow_ = h.row; selClip_ = h.clipIdx; selMidi_ = false;
      emit playlist_->selectionChanged();
      auto& t = s_.tracks()[selRow_];
      drag_.origStart = t.clips[h.clipIdx].startBeats;
      drag_.origDur   = t.clips[h.clipIdx].durationBeats;
      drag_.kind = Drag::Resize;
      drag_.beat0 = double(e->pos().x() - kLabelW) / pxPerBeat_;
      drag_.right = true;
      update();
      return;
    }
    // empty lane → start marquee
    if (h.row >= 0) {
      selRow_ = h.row; selClip_ = -1; selMidi_ = false;
      emit playlist_->trackSelected(trackIdAt(selRow_));
      emit playlist_->selectionChanged();
      drag_.kind = Drag::Marquee;
      drag_.origStart = -1;
      drag_.origDur   =  0;
      update();
    }
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    if (drag_.kind == Drag::None) return;
    QPoint pt = e->pos();
    double delta = double(pt.x() - drag_.pressPt.x()) / pxPerBeat_;
    double beatDelta = double(pt.x() - drag_.lastPt.x()) / pxPerBeat_;
    drag_.lastPt = pt;
    if (drag_.kind == Drag::Move && drag_.row >= 0 && !drag_.midi) {
      auto& t = s_.tracks()[drag_.row];
      if (drag_.clipIdx >= 0 && drag_.clipIdx < (int)t.clips.size()) {
        auto& c = t.clips[drag_.clipIdx];
        double origStart = drag_.origStart;
        double newStart = quant(origStart + delta);
        c.startBeats = clamp0(newStart);
      }
    }
    if (drag_.kind == Drag::Resize && drag_.row >= 0 && !drag_.midi) {
      auto& t = s_.tracks()[drag_.row];
      if (drag_.clipIdx >= 0 && drag_.clipIdx < (int)t.clips.size()) {
        auto& c = t.clips[drag_.clipIdx];
        c.durationBeats = qMax(kMinDur, quant(drag_.origDur + delta));
      }
    }
    update();
    playlist_->update();
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    if (drag_.kind == Drag::Move || drag_.kind == Drag::Resize) {
      emit playlist_->edited();
    }
    drag_.kind = Drag::None;
    update();
    playlist_->update();
  }

public:
  void keyPressEvent(QKeyEvent* e) override {
    if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) &&
        selRow_ >= 0 && selClip_ >= 0 && !selMidi_) {
      auto& t = s_.tracks()[selRow_];
      if (selClip_ < (int)t.clips.size())
        t.clips.erase(t.clips.begin() + selClip_);
      selClip_ = -1;
      emit playlist_->edited();
      emit playlist_->selectionChanged();
      update();
      return;
    }
    if (e->key() == Qt::Key_D && e->modifiers().testFlag(Qt::ControlModifier) &&
        selRow_ >= 0 && selClip_ >= 0 && !selMidi_) {
      auto& t = s_.tracks()[selRow_];
      if (selClip_ < (int)t.clips.size()) {
        auto c = t.clips[selClip_];
        c.name += " Copy";
        c.startBeats += c.durationBeats;
        t.clips.push_back(c);
        emit playlist_->edited();
      }
      update();
      return;
    }
    if (e->key() == Qt::Key_S && e->modifiers().testFlag(Qt::ControlModifier) &&
        selRow_ >= 0 && selClip_ >= 0 && !selMidi_) {
      // split at playhead
      auto& t = s_.tracks()[selRow_];
      if (selClip_ < (int)t.clips.size()) {
        auto& c = t.clips[selClip_];
        if (playhead_ > c.startBeats && playhead_ < c.startBeats + c.durationBeats) {
          auto right = c;
          double splitPoint = playhead_ - c.startBeats;
          right.startBeats = playhead_;
          right.durationBeats -= splitPoint;
          c.durationBeats = splitPoint;
          t.clips.push_back(right);
          emit playlist_->edited();
        }
      }
      update();
      return;
    }
    QWidget::keyPressEvent(e);
  }

  void wheelEvent(QWheelEvent* e) override {
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
      double delta = e->angleDelta().y() > 0 ? 1.1 : 1/1.1;
      pxPerBeat_ = qBound(4.0, pxPerBeat_ * delta, 128.0);
      updateGeometry();
      playlist_->update();
    } else {
      QWidget::wheelEvent(e);
    }
  }

  void dragEnterEvent(QDragEnterEvent* e) override {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
  }
  void dropEvent(QDropEvent* e) override {
    if (!e->mimeData()->hasUrls()) return;
    for (auto& url : e->mimeData()->urls()) {
      if (!url.isLocalFile()) continue;
      double beat = double(e->pos().x() - kLabelW) / pxPerBeat_;
      int row = qMax(0, drag_.row);
      playlist_->insertSampleAt(url.toLocalFile(), quant(beat), row);
    }
    drag_.kind = Drag::None;
  }

  void contextMenuEvent(QContextMenuEvent* e) override {
    Hit h = hitTest(e->pos());
    if (h.row < 0) return;
    QMenu menu;
    auto& t = s_.tracks()[h.row];
    if (h.kind == Hit::Clip && h.clipIdx >= 0 && !h.midi) {
      menu.addAction("Rename...");
      menu.addAction("Duplicate");
      menu.addAction("Split at Playhead");
      menu.addAction(t.clips[h.clipIdx].gainDb < -90.f ? "Unmute" : "Mute");
      menu.addAction("Open in Piano Roll");
      menu.addAction("Delete");
    } else if (h.kind == Hit::Track) {
      menu.addAction("Rename Track...");
    } else return;

    QAction* a = menu.exec(e->globalPos());
    if (!a) return;
    QString txt = a->text();
    if (txt == "Rename Track..." && h.row >= 0) {
      bool ok; QString n = QInputDialog::getText(this, "Rename Track", "Name:",
                                                 QLineEdit::Normal, QString::fromStdString(t.name), &ok);
      if (ok && !n.isEmpty()) { t.name = n.toStdString(); emit playlist_->edited(); update(); }
    }
    if (txt == "Rename..." && h.clipIdx >= 0) {
      bool ok; QString n = QInputDialog::getText(this, "Rename Clip", "Name:",
                                                 QLineEdit::Normal, clipName(t.clips[h.clipIdx]), &ok);
      if (ok && !n.isEmpty()) { t.clips[h.clipIdx].name = n.toStdString(); emit playlist_->edited(); update(); }
    }
    if (txt == "Duplicate" && h.clipIdx >= 0) {
      auto c = t.clips[h.clipIdx];
      c.name += " Copy";
      c.startBeats += c.durationBeats;
      t.clips.push_back(c);
      emit playlist_->edited(); update();
    }
    if (txt == "Split at Playhead" && h.clipIdx >= 0) {
      auto& c = t.clips[h.clipIdx];
      if (playhead_ > c.startBeats && playhead_ < c.startBeats + c.durationBeats) {
        auto right = c;
        right.startBeats = playhead_;
        right.durationBeats = c.startBeats + c.durationBeats - playhead_;
        c.durationBeats = playhead_ - c.startBeats;
        t.clips.push_back(right);
        emit playlist_->edited(); update();
      }
    }
    if (txt == "Mute" && h.clipIdx >= 0) {
      t.clips[h.clipIdx].gainDb = -96.f; emit playlist_->edited(); update();
    }
    if (txt == "Unmute" && h.clipIdx >= 0) {
      t.clips[h.clipIdx].gainDb = 0.f;   emit playlist_->edited(); update();
    }
    if (txt == "Open in Piano Roll" && h.row >= 0) {
      emit playlist_->openInPianoRoll(QString::fromStdString(t.id));
    }
    if (txt == "Delete" && h.clipIdx >= 0 && h.row >= 0) {
      t.clips.erase(t.clips.begin() + h.clipIdx);
      selClip_ = -1;
      emit playlist_->edited(); emit playlist_->selectionChanged(); update();
    }
  }

  void resizeEvent(QResizeEvent*) override {
    updateGeometry();
  }
};

// ───────────────────────── Playlist (outer) ─────────────────────────
Playlist::Playlist(misuc::project::Session& session, QWidget* parent)
  : QWidget(parent), s_(session) {

  auto& te = ThemeEngine::instance();
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // ── toolbar (icon-only matte-glass chrome, §36-38/§66-68)
  auto* tb = new QWidget(this);
  tb->setFixedHeight(36);
  tb->setStyleSheet(QString("background: %1; border-bottom: 1px solid %2;")
      .arg(theme::kChrome.name(QColor::HexArgb),
           theme::kChromeBorder.name(QColor::HexArgb)));
  auto* tbLay = new QHBoxLayout(tb);
  tbLay->setContentsMargins(10, 4, 10, 4);
  tbLay->setSpacing(4);

  auto mkBtn = [&](icons::IconId id, const QString& tip, QKeySequence sc) {
    auto* b = new QToolButton(tb);
    b->setIcon(icons::make(id, theme::kText, 16));
    b->setIconSize(QSize(16,16));
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setFixedSize(28,28);
    b->setShortcut(sc);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
  };

  selBtn_  = mkBtn(icons::IconId::Select, "Select (V)", QKeySequence(Qt::Key_V));
  sliceBtn_= mkBtn(icons::IconId::Slice,  "Slice (C)",  QKeySequence(Qt::Key_C));
  snapBtn_ = mkBtn(icons::IconId::Snap,   "Snap (N)",   QKeySequence(Qt::Key_N));
  addBtn_  = mkBtn(icons::IconId::Add,    "Import Audio",QKeySequence(Qt::CTRL | Qt::Key_I));
  tbLay->addWidget(selBtn_); tbLay->addWidget(sliceBtn_); tbLay->addSpacing(8);
  tbLay->addWidget(snapBtn_); tbLay->addSpacing(8);
  tbLay->addWidget(addBtn_); tbLay->addStretch(1);

  zoom_ = new QSlider(Qt::Horizontal, tb);
  zoom_->setRange(4, 128);
  zoom_->setValue(24);
  zoom_->setFixedWidth(120);
  zoom_->setToolTip("Zoom (Ctrl+Wheel)");
  zoom_->setFocusPolicy(Qt::NoFocus);
  zoomIcon_ = new QToolButton(tb);
  zoomIcon_->setIcon(icons::make(icons::IconId::ZoomIn, theme::kTextDim, 15));
  zoomIcon_->setIconSize(QSize(15, 15));
  zoomIcon_->setAutoRaise(true);
  zoomIcon_->setFixedSize(24, 24);
  zoomIcon_->setCursor(Qt::PointingHandCursor);
  zoomIcon_->setFocusPolicy(Qt::NoFocus);
  tbLay->addWidget(zoomIcon_);
  tbLay->addWidget(zoom_);
  lay->addWidget(tb);

  connect(selBtn_, &QToolButton::toggled, this, [=](bool on){ if(on) { sliceBtn_->setChecked(false); } });
  connect(sliceBtn_, &QToolButton::toggled, this, [=](bool on){ if(on) { selBtn_->setChecked(false); } });
  connect(snapBtn_, &QToolButton::toggled, this, &Playlist::setSnap);
  connect(addBtn_,  &QToolButton::clicked, this, &Playlist::importAudioDialog);
  connect(zoom_, &QSlider::valueChanged, this, [this](int v){ applyZoom(v); });

  // ── grid inside scroll area
  grid_ = new PlaylistGrid(s_, this, nullptr);
  auto* sa = new QScrollArea(this);
  sa->setWidgetResizable(true);
  sa->setWidget(grid_);
  sa->setStyleSheet(QString("background: %1;").arg(theme::kBackground.name()));
  lay->addWidget(sa);
}

void Playlist::keyPressEvent(QKeyEvent* e) { grid_->keyPressEvent(e); }

void Playlist::importAudioDialog() {
  QStringList files = QFileDialog::getOpenFileNames(this, "Import Audio", {},
      "Audio files (*.wav *.flac *.ogg *.mp3);;All files (*)");
  if (files.isEmpty()) return;
  for (auto& f : files) {
    double beat = 0;
    for (auto& t : s_.tracks())
      for (auto& c : t.clips)
        beat = qMax(beat, c.startBeats + c.durationBeats);
    insertSampleAt(f, beat, qMax(0, grid_->selRow_));
  }
  emit edited();
  grid_->update();
}

void Playlist::applyZoom(double v) {
  grid_->pxPerBeat_ = v;
  grid_->updateGeometry();
  grid_->update();
  update();
}

void Playlist::refresh() { grid_->update(); }
void Playlist::feedPlayhead(bool playing, double beats) {
  grid_->playing_ = playing;
  grid_->playhead_ = beats;
  grid_->update();
}
void Playlist::setSnap(bool on) { grid_->snapOn_ = on; }
void Playlist::setSongMode(bool song) { grid_->songMode_ = song; }
void Playlist::selectTrack(const QString& id) {
  grid_->selRow_ = grid_->trackRow(id);
  grid_->selClip_ = -1;
  grid_->selMidi_ = false;
  grid_->update();
}
void Playlist::clearSelection() {
  grid_->selRow_ = -1; grid_->selClip_ = -1; grid_->selMidi_ = false; grid_->update();
}

void Playlist::attachHints(HintManager* hints) {
  if (!hints)
    return;
  hints->registerTool(selBtn_, QStringLiteral("Tool: Select"),
                      QStringLiteral("Select and move clips and MIDI regions"), QKeySequence(Qt::Key_V).toString(QKeySequence::NativeText), icons::IconId::Select);
  hints->registerTool(sliceBtn_, QStringLiteral("Tool: Slice"),
                      QStringLiteral("Split the clip under the playhead into two"), QKeySequence(Qt::Key_C).toString(QKeySequence::NativeText), icons::IconId::Slice);
  hints->registerTool(snapBtn_, QStringLiteral("Snap to Grid"),
                      QStringLiteral("Quantize clip moves and resizes to the beat grid"), QKeySequence(Qt::Key_N).toString(QKeySequence::NativeText), icons::IconId::Snap, [this] {
                        return grid_->snapOn_ ? QStringLiteral("on") : QStringLiteral("off");
                      });
  hints->registerTool(addBtn_, QStringLiteral("Import Audio"),
                      QStringLiteral("Import a sample into the selected track as a new clip"), QKeySequence(QStringLiteral("Ctrl+I")).toString(QKeySequence::NativeText), icons::IconId::Add);
  hints->registerTool(zoomIcon_, QStringLiteral("Zoom"),
                      QStringLiteral("Horizontal zoom of the timeline"), QString(), icons::IconId::ZoomIn);
  hints->registerTool(zoom_, QStringLiteral("Zoom"),
                      QStringLiteral("Horizontal zoom of the timeline (Ctrl+Wheel also works)"), QString(), icons::IconId::ZoomIn, [this] {
                        return QStringLiteral("%1 px/beat").arg(grid_->pxPerBeat_, 0, 'f', 1);
                      });
}
QString Playlist::selectedTrackId() const { return grid_->trackIdAt(grid_->selRow_); }
int Playlist::selectedClipRow() const { return grid_->selRow_; }
bool Playlist::hasSelection() const { return grid_->selRow_ >= 0; }

Playlist::SelectionSummary Playlist::selectionSummary() const {
  SelectionSummary out;
  const int row = grid_->selRow_;
  if (row < 0 || row >= (int)s_.tracks().size()) return out;
  const auto& t = s_.tracks()[row];
  out.trackRow = row;
  out.trackId = QString::fromStdString(t.id);
  out.trackName = QString::fromStdString(t.name);
  out.trackMuted = t.muted;
  out.trackSolo = t.solo;
  out.trackGainDb = t.gainDb;
  out.trackPan = t.pan;
  out.hasClips = !t.clips.empty();
  if (grid_->selClip_ >= 0 && !grid_->selMidi_ &&
      grid_->selClip_ < (int)t.clips.size()) {
    const auto& c = t.clips[grid_->selClip_];
    out.clipIndex = grid_->selClip_;
    out.clipName = clipName(c);
    out.clipSource = QString::fromStdString(c.sourceFile);
    out.startBeats = c.startBeats;
    out.durationBeats = c.durationBeats;
    out.gainDb = c.gainDb;
    out.clipOffsetBeats = c.clipOffsetBeats;
  } else if (grid_->selMidi_) {
    out.midiRegion = true;
    auto mr = grid_->midiRegion(t);
    out.noteCount = mr.count;
    out.startBeats = mr.start;
    out.durationBeats = mr.end - mr.start;
  } else {
    out.noteCount = int(t.notes.size());
  }
  return out;
}

void Playlist::insertSampleAt(const QString& source, double startBeat, int trackRow) {
  misuc::project::AudioClip c;
  c.sourceFile = source.toStdString();
  c.name = source.section(QLatin1Char('/'), -1).toStdString();
  c.startBeats = startBeat;
  c.durationBeats = 4;
  if (trackRow >= 0 && trackRow < (int)s_.tracks().size()) {
    auto& t = s_.tracks()[trackRow];
    t.clips.push_back(c);
  }
  emit edited();
  grid_->update();
}

} // namespace misuc::ui