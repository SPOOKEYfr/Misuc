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
static constexpr int kPlayheadAlpha = 180;
static constexpr double kMinDur = 0.1;   // minimum clip/region duration (beats)
static constexpr int kResizeHit = 6;     // edge grab band, px

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
  bool sliceTool_ = false;

  int selRow_ = -1;
  int selClip_ = -1;    // audio-clip index
  int selRegion_ = -1;  // real MIDI region index

  // One selectable item; either a clip (clipIdx) OR a region (regionIdx).
  struct Item { int row = -1; int clipIdx = -1; int regionIdx = -1; };
  std::vector<Item> multi_;  // marquee multi-selection

  // clipboard for copy/paste
  struct ClipItem { int row; bool midi; int clipIdx; int regionIdx; double relStart; };
  std::vector<ClipItem> clipBoard_;
  double clipAnchor_ = -1;

  // drag state
  struct Drag {
    enum Kind { None, MoveClip, ResizeClip, TrimClip, MoveRegion,
                ResizeRegion, TrimRegion, Slice, Marquee, SeekHead };
    Kind kind = None; int row = -1; int clipIdx = -1; int regionIdx = -1;
    double origStart = 0, origDur = 0;
    QPoint pressPt, lastPt; double beat0 = 0;
    // Aggregated move set: when the user drags an item that belongs to a
    // multi-selection, every selected item shares the same delta (lockstep).
    std::vector<Item> dragItems_;
    std::vector<double> origStarts_;
  };
  Drag drag_;

  QHash<QString, QPixmap> waveformPixCache_;

  PlaylistGrid(misuc::project::Session& session, Playlist* playlist, QWidget* parent = nullptr)
    : QWidget(parent), s_(session), playlist_(playlist) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(kLabelW + 200);
  }

  int rowCount() const { return qMax(1, (int)s_.tracks().size()); }

  double maxEnd() const {
    double maxEnd = 32;
    for (auto& t : s_.tracks()) {
      for (auto& c : t.clips)
        maxEnd = qMax(maxEnd, c.startBeats + c.durationBeats);
      for (auto& r : t.regions)
        maxEnd = qMax(maxEnd, r.startBeats + r.durationBeats);
    }
    return maxEnd;
  }

  QSize sizeHint() const override {
    int w = int(kLabelW + (maxEnd() + 4) * pxPerBeat_);
    return QSize(qMax(w, 600), rowCount() * kRowH + kRulerH);
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

  double itemStart(const Item& it) const {
    if (it.row < 0 || it.row >= (int)s_.tracks().size()) return 0;
    const auto& t = s_.tracks()[it.row];
    if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size())
      return t.clips[it.clipIdx].startBeats;
    if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size())
      return t.regions[it.regionIdx].startBeats;
    return 0;
  }
  double itemDur(const Item& it) const {
    if (it.row < 0 || it.row >= (int)s_.tracks().size()) return 0;
    const auto& t = s_.tracks()[it.row];
    if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size())
      return t.clips[it.clipIdx].durationBeats;
    if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size())
      return t.regions[it.regionIdx].durationBeats;
    return 0;
  }
  Item primaryItem() const {
    Item it; it.row = selRow_;
    it.clipIdx = selClip_; it.regionIdx = selRegion_;
    return it;
  }
  std::vector<Item> selectedItems() const {
    if (!multi_.empty()) return multi_;
    Item it = primaryItem();
    if (it.row < 0) return {};
    return {it};
  }
  bool isItemSelected(const Item& it) const {
    if (!multi_.empty())
      return it.row == multi_[0].row; // fallback only
    return it.row == selRow_ &&
           (it.clipIdx >= 0 ? it.clipIdx == selClip_ : it.regionIdx == selRegion_);
  }

  // ── hit test ──
  struct Hit {
    enum Kind { None, Track, Clip, Region, ResizeRightClip, TrimLeftClip,
                ResizeRightRegion, TrimLeftRegion, Mute, Solo };
    Kind kind = None; int row = -1; int clipIdx = -1; int regionIdx = -1;
  };
  Hit hitTest(QPoint pt) const {
    Hit h;
    int contentH = rowCount() * kRowH + kRulerH;
    int contentW = int(kLabelW + (maxEnd() + 4) * pxPerBeat_);
    if (pt.y() < kRulerH || pt.x() < 0 || pt.y() > contentH || pt.x() > contentW)
      return h;
    int row = (pt.y() - kRulerH) / kRowH;
    if (row < 0 || row >= (int)s_.tracks().size()) return h;
    h.row = row;
    if (pt.x() < kLabelW) {
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
    // Audio clips first (they can overlap regions but carry a waveform body).
    auto& t = s_.tracks()[row];
    for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
      auto& c = t.clips[ci];
      double x0 = kLabelW + c.startBeats * pxPerBeat_;
      double x1 = kLabelW + (c.startBeats + c.durationBeats) * pxPerBeat_;
      if (pt.x() >= x0 && pt.x() <= x1) {
        h.clipIdx = ci;
        if (pt.x() >= x1 - kResizeHit) h.kind = Hit::ResizeRightClip;
        else if (pt.x() <= x0 + kResizeHit) h.kind = Hit::TrimLeftClip;
        else h.kind = Hit::Clip;
        return h;
      }
    }
    // Real MIDI regions.
    for (int ri = 0; ri < (int)t.regions.size(); ++ri) {
      auto& r = t.regions[ri];
      double x0 = kLabelW + r.startBeats * pxPerBeat_;
      double x1 = kLabelW + (r.startBeats + r.durationBeats) * pxPerBeat_;
      if (pt.x() >= x0 && pt.x() <= x1) {
        h.regionIdx = ri;
        if (pt.x() >= x1 - kResizeHit) h.kind = Hit::ResizeRightRegion;
        else if (pt.x() <= x0 + kResizeHit) h.kind = Hit::TrimLeftRegion;
        else h.kind = Hit::Region;
        return h;
      }
    }
    return h;
  }

  // ── selection plumbing ──
  void selectSingle(int row, int clipIdx, int regionIdx) {
    selRow_ = row; selClip_ = clipIdx; selRegion_ = regionIdx;
    multi_.clear();
    emit playlist_->selectionChanged();
  }
  bool inMulti(int row, int clipIdx, int regionIdx) const {
    for (auto& it : multi_)
      if (it.row == row && it.clipIdx == clipIdx && it.regionIdx == regionIdx)
        return true;
    return false;
  }
  void updateMarquee() {
    multi_.clear();
    QRectF box = QRectF(drag_.pressPt, drag_.lastPt).normalized();
    for (int row = 0; row < (int)s_.tracks().size(); ++row) {
      const auto& t = s_.tracks()[row];
      const double y0 = kRulerH + row * kRowH;
      if (box.bottom() < y0 || box.top() > y0 + kRowH) continue;
      for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
        const double x0 = kLabelW + t.clips[ci].startBeats * pxPerBeat_;
        const double x1 = x0 + t.clips[ci].durationBeats * pxPerBeat_;
        QRectF r(x0, y0, x1 - x0, kRowH);
        if (r.intersects(box)) multi_.push_back({row, ci, -1});
      }
      for (int ri = 0; ri < (int)t.regions.size(); ++ri) {
        const double x0 = kLabelW + t.regions[ri].startBeats * pxPerBeat_;
        const double x1 = x0 + t.regions[ri].durationBeats * pxPerBeat_;
        QRectF r(x0, y0, x1 - x0, kRowH);
        if (r.intersects(box)) multi_.push_back({row, -1, ri});
      }
    }
    emit playlist_->selectionChanged();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    int W = width(), H = height();
    int rows = rowCount();

    p.fillRect(rect(), theme::kBackground);

    // ── ruler
    p.fillRect(0, 0, W, kRulerH, theme::kPanelHeader);
    p.setPen(theme::kBorderStrong);
    p.drawLine(0, kRulerH - 1, W, kRulerH - 1);
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

    for (int r = 0; r < rows; ++r) {
      int y = kRulerH + r * kRowH;
      QColor laneBg = (r % 2 == 0) ? theme::kMatteSurface : theme::kBackground;
      if (r == selRow_) {
        QColor sel = laneBg;
        sel.setAlpha(60);
        laneBg = sel;
      }
      p.fillRect(kLabelW, y, W - kLabelW, kRowH, laneBg);
      p.setPen(theme::kGridRow);
      p.drawLine(kLabelW, y + kRowH - 1, W, y + kRowH - 1);

      auto& t = s_.tracks()[r];
      QString name = QString::fromStdString(t.name);
      p.setPen(theme::kText);
      QFont f = p.font(); f.setPixelSize(13); p.setFont(f);
      p.drawText(10, y + kRowH / 2 + 5, name.left(16));

      int iconY = y + (kRowH - 18) / 2;
      int mx = kLabelW - 42;
      p.setPen(Qt::NoPen);
      p.setBrush(t.muted ? theme::kAccent : theme::kBorderStrong);
      p.drawEllipse(mx, iconY, 18, 18);
      if (t.muted) {
        p.setPen(QPen(theme::kText, 2));
        p.drawLine(mx + 4, iconY + 9, mx + 14, iconY + 9);
      }
      int sx = mx + 22;
      p.setPen(Qt::NoPen);
      p.setBrush(t.solo ? theme::kAccent : theme::kBorderStrong);
      p.drawEllipse(sx, iconY, 18, 18);
      if (t.solo) {
        p.setPen(QPen(theme::kText, 2));
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
        bool sel = (r == selRow_ && ci == selClip_) || inMulti(r, ci, -1);
        QColor bg = isMidi(c) ? theme::kClipMidi : theme::kClipBody;
        if (c.gainDb < -90.f) bg.setAlpha(120);
        p.setPen(sel ? theme::kAccent : theme::kGridStrong);
        p.setBrush(bg);
        p.drawRoundedRect(body, 4, 4);
        if (sel) {
          p.setPen(QPen(theme::kOk, 1, Qt::DashLine));
          p.drawRoundedRect(body.adjusted(1, 1, -1, -1), 4, 4);
        }
        p.setPen(theme::kText);
        QFont cf = p.font(); cf.setPixelSize(11); p.setFont(cf);
        p.drawText(body.adjusted(6, 2, -6, -2), Qt::TextSingleLine, clipName(c));
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
        if (sel) {
          p.setPen(Qt::NoPen);
          p.setBrush(theme::kOk);
          double e = 3.0;
          p.drawRect(QRectF(body.left() + 2, body.top() + 8, e, body.height() - 16));
          p.drawRect(QRectF(body.right() - e - 2, body.top() + 8, e, body.height() - 16));
        }
        p.setFont(f);
      }

      // ── real MIDI regions
      for (int ri = 0; ri < (int)t.regions.size(); ++ri) {
        auto& rg = t.regions[ri];
        double x0 = kLabelW + rg.startBeats * pxPerBeat_;
        double w  = rg.durationBeats * pxPerBeat_;
        double x1 = x0 + w;
        if (x1 < kLabelW || x0 > W) continue;
        QRectF body(x0, y + 4, w, kRowH - 8);
        bool sel = (r == selRow_ && ri == selRegion_) || inMulti(r, -1, ri);
        QColor mBg = theme::kClipMidi;
        p.setPen(sel ? theme::kAccent : theme::kGridStrong);
        p.setBrush(mBg);
        p.drawRoundedRect(body, 4, 4);
        if (sel) {
          p.setPen(QPen(theme::kOk, 1, Qt::DashLine));
          p.drawRoundedRect(body.adjusted(1, 1, -1, -1), 4, 4);
        }
        // mini note bars (region-local pitch bands)
        int noteH = 4;
        p.setPen(Qt::NoPen);
        QColor noteCol = theme::kText; noteCol.setAlpha(70);
        p.setBrush(noteCol);
        for (auto& n : rg.notes) {
          double nx = body.left() + n.startBeats * pxPerBeat_;
          double nw = n.durationBeats * pxPerBeat_;
          int pitchIdx = qBound(0, n.pitch % 8, 7);
          double ny = body.top() + (pitchIdx / 8.0) * (body.height() - noteH);
          p.drawRect(nx, ny, qMax(nw, 2.0), noteH);
        }
        p.setPen(theme::kText);
        QFont mf = p.font(); mf.setPixelSize(10); p.setFont(mf);
        QString label = QString::fromStdString(rg.name);
        if (label.isEmpty()) label = QStringLiteral("MIDI");
        label += QStringLiteral(" · %1 notes").arg((int)rg.notes.size());
        p.drawText(body.adjusted(6, 1, -6, -1), Qt::TextSingleLine, label);
        if (sel) {
          p.setPen(Qt::NoPen);
          p.setBrush(theme::kOk);
          double e = 3.0;
          p.drawRect(QRectF(body.left() + 2, body.top() + 8, e, body.height() - 16));
          p.drawRect(QRectF(body.right() - e - 2, body.top() + 8, e, body.height() - 16));
        }
        p.setFont(f);
      }
    }

    // ── playhead
    if (playhead_ >= 0) {
      int px = kLabelW + int(playhead_ * pxPerBeat_);
      QColor ph = theme::kPlayhead;
      p.setPen(QPen(ph, 2));
      p.drawLine(px, kRulerH, px, H);
      QPolygonF tri;
      tri << QPointF(px - 5, 0) << QPointF(px + 5, 0) << QPointF(px, 9);
      p.setPen(Qt::NoPen); p.setBrush(ph);
      p.drawPolygon(tri);
    }

    if (drag_.kind == Drag::Marquee) {
      QRectF box = QRectF(drag_.pressPt, drag_.lastPt).normalized();
      QColor m = theme::kSelection; m.setAlpha(40);
      p.setBrush(m);
      p.setPen(QPen(theme::kAccent, 1, Qt::DashLine));
      p.drawRect(box);
    }
  }

  // ── region model helpers ──
  void sliceRegion(int row, int regionIdx, double splitGlobal) {
    if (row < 0 || regionIdx < 0 || row >= (int)s_.tracks().size()) return;
    auto& t = s_.tracks()[row];
    if (regionIdx >= (int)t.regions.size()) return;
    misuc::project::MidiRegion& r = t.regions[regionIdx];
    const std::string tid = t.id;
    const double rel = splitGlobal - r.startBeats;
    if (rel <= 0.0 || rel >= r.durationBeats) return;
    const double origDur = r.durationBeats;
    const std::string rightId = s_.duplicateRegion(tid, r.id);
    misuc::project::MidiRegion* right = s_.findRegion(tid, rightId);
    if (!right) return;
    right->startBeats = splitGlobal;
    right->durationBeats = origDur - rel;
    std::vector<misuc::project::NoteEvent> leftNotes, rightNotes;
    for (auto& n : r.notes) {
      if (n.startBeats >= rel) {
        misuc::project::NoteEvent rn = n;
        rn.startBeats -= rel;
        rightNotes.push_back(rn);
      } else {
        leftNotes.push_back(n);
      }
    }
    r.durationBeats = rel;
    r.notes = std::move(leftNotes);
    right->notes = std::move(rightNotes);
  }

  int sliceClip(int row, int clipIdx, double splitGlobal) {
    if (row < 0 || clipIdx < 0 || row >= (int)s_.tracks().size()) return clipIdx;
    auto& t = s_.tracks()[row];
    if (clipIdx >= (int)t.clips.size()) return clipIdx;
    auto& c = t.clips[clipIdx];
    const double rel = splitGlobal - c.startBeats;
    if (rel <= 0.0 || rel >= c.durationBeats) return clipIdx;
    auto right = c;
    right.id.clear(); // Session::addClip assigns a fresh unique id
    right.startBeats = splitGlobal;
    right.durationBeats = c.durationBeats - rel;
    c.durationBeats = rel;
    s_.addClip(t.id, std::move(right));
    return (int)t.clips.size() - 1;
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    drag_.pressPt = e->pos();
    drag_.lastPt  = e->pos();
    drag_.dragItems_.clear();
    drag_.origStarts_.clear();

    // Grab the playhead: within ~5px of its line (ruler triangle or through
    // the lanes) pressing and dragging scrubs the transport caret directly.
    const int phx = kLabelW + static_cast<int>(playhead_ * pxPerBeat_);
    if (e->pos().x() >= kLabelW && std::abs(e->pos().x() - phx) <= 5) {
      double beat = qMax(0.0, (e->pos().x() - kLabelW) / pxPerBeat_);
      if (snapOn_) beat = quant(beat);
      playhead_ = beat;
      drag_.kind = Drag::SeekHead;
      emit playlist_->seekRequested(beat);
      update();
      return;
    }

    Hit h = hitTest(e->pos());
    drag_.row = h.row;
    drag_.clipIdx = h.clipIdx;
    drag_.regionIdx = h.regionIdx;

    if (h.row < 0) {
      // ruler click → seek (snapped when snap is on), keep dragging to scrub
      if (e->pos().y() < kRulerH && e->pos().x() >= kLabelW) {
        double beat = qMax(0.0, (e->pos().x() - kLabelW) / pxPerBeat_);
        if (snapOn_) beat = quant(beat);
        playhead_ = beat;
        drag_.kind = Drag::SeekHead;
        emit playlist_->seekRequested(beat);
        update();
      }
      return;
    }

    if (h.kind == Hit::Mute) {
      auto& t = s_.tracks()[h.row];
      t.muted = !t.muted;
      emit playlist_->edited(); update(); return;
    }
    if (h.kind == Hit::Solo) {
      auto& t = s_.tracks()[h.row];
      t.solo = !t.solo;
      emit playlist_->edited(); update(); return;
    }
    if (h.kind == Hit::Track) {
      selRow_ = h.row; selClip_ = -1; selRegion_ = -1; multi_.clear();
      emit playlist_->trackSelected(trackIdAt(selRow_));
      emit playlist_->selectionChanged(); update(); return;
    }

    // Slice tool slices whatever is under the pointer.
    if (sliceTool_) {
      double beat = qMax(0.0, (e->pos().x() - kLabelW) / pxPerBeat_);
      if (h.kind == Hit::Clip || h.kind == Hit::ResizeRightClip ||
          h.kind == Hit::TrimLeftClip) {
        int idx = sliceClip(h.row, h.clipIdx, quant(beat));
        selRow_ = h.row; selClip_ = idx; selRegion_ = -1;
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged();
        emit playlist_->edited(); update(); return;
      }
      if (h.kind == Hit::Region || h.kind == Hit::ResizeRightRegion ||
          h.kind == Hit::TrimLeftRegion) {
        sliceRegion(h.row, h.regionIdx, quant(beat));
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged();
        emit playlist_->edited(); update(); return;
      }
      return;
    }

    const bool ctrl = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
    if (h.kind == Hit::ResizeRightClip || h.kind == Hit::Clip ||
        h.kind == Hit::TrimLeftClip) {
      if (ctrl) {
        if (inMulti(h.row, h.clipIdx, -1))
          multi_.erase(std::remove_if(multi_.begin(), multi_.end(),
              [&](const Item& it){ return it.row == h.row && it.clipIdx == h.clipIdx; }),
              multi_.end());
        else {
          multi_.push_back({h.row, h.clipIdx, -1});
          selRow_ = h.row; selClip_ = h.clipIdx; selRegion_ = -1;
        }
        emit playlist_->selectionChanged(); update(); return;
      }
      if (!inMulti(h.row, h.clipIdx, -1))
        selectSingle(h.row, h.clipIdx, -1);
      else {
        // The pressed clip is already part of a marquee selection: keep the
        // whole group so a drag moves every selected item in lockstep.
        selRow_ = h.row; selClip_ = h.clipIdx; selRegion_ = -1;
      }
      emit playlist_->trackSelected(trackIdAt(selRow_));
      auto& c = s_.tracks()[h.row].clips[h.clipIdx];
      drag_.origStart = c.startBeats;
      drag_.origDur   = c.durationBeats;
      for (auto& it : selectedItems()) {
        drag_.dragItems_.push_back(it);
        drag_.origStarts_.push_back(itemStart(it));
      }
      drag_.kind = h.kind == Hit::ResizeRightClip ? Drag::ResizeClip
                  : h.kind == Hit::TrimLeftClip  ? Drag::TrimClip
                                                 : Drag::MoveClip;
      update(); return;
    }
    if (h.kind == Hit::ResizeRightRegion || h.kind == Hit::Region ||
        h.kind == Hit::TrimLeftRegion) {
      if (ctrl) {
        if (inMulti(h.row, -1, h.regionIdx))
          multi_.erase(std::remove_if(multi_.begin(), multi_.end(),
              [&](const Item& it){ return it.row == h.row && it.regionIdx == h.regionIdx; }),
              multi_.end());
        else {
          multi_.push_back({h.row, -1, h.regionIdx});
          selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
        }
        emit playlist_->selectionChanged(); update(); return;
      }
      if (!inMulti(h.row, -1, h.regionIdx))
        selectSingle(h.row, -1, h.regionIdx);
      else {
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
      }
      emit playlist_->trackSelected(trackIdAt(selRow_));
      auto& rg = s_.tracks()[h.row].regions[h.regionIdx];
      drag_.origStart = rg.startBeats;
      drag_.origDur   = rg.durationBeats;
      for (auto& it : selectedItems()) {
        drag_.dragItems_.push_back(it);
        drag_.origStarts_.push_back(itemStart(it));
      }
      drag_.kind = h.kind == Hit::ResizeRightRegion ? Drag::ResizeRegion
                  : h.kind == Hit::TrimLeftRegion ? Drag::TrimRegion
                                                  : Drag::MoveRegion;
      update(); return;
    }
    if (h.row >= 0) {
      // empty lane → marquee multi-select
      selRow_ = h.row; selClip_ = -1; selRegion_ = -1;
      emit playlist_->trackSelected(trackIdAt(selRow_));
      emit playlist_->selectionChanged();
      drag_.kind = Drag::Marquee;
      update();
    }
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    QPoint pt = e->pos();
    if (drag_.kind == Drag::None) {
      // Hover affordances: horizontal-resize cursor on the ruler and on the
      // playhead line (slice tool keeps its crosshair).
      if (sliceTool_) {
        setCursor(Qt::CrossCursor);
      } else if ((pt.y() < kRulerH && pt.x() >= kLabelW)) {
        setCursor(Qt::SizeHorCursor);
      } else {
        const int phx = kLabelW + static_cast<int>(playhead_ * pxPerBeat_);
        if (pt.x() >= kLabelW && std::abs(pt.x() - phx) <= 5)
          setCursor(Qt::SizeHorCursor);
        else
          unsetCursor();
      }
      return;
    }
    if (drag_.kind == Drag::SeekHead) {
      double beat = qMax(0.0, (pt.x() - kLabelW) / pxPerBeat_);
      if (snapOn_) beat = quant(beat);
      if (std::abs(beat - playhead_) > 1e-9) {
        playhead_ = beat;
        emit playlist_->seekRequested(beat);
        update();
        playlist_->update();
      }
      return;
    }
    double delta = double(pt.x() - drag_.pressPt.x()) / pxPerBeat_;
    drag_.lastPt = pt;
    if (drag_.kind == Drag::MoveClip || drag_.kind == Drag::MoveRegion) {
      // Aggregated move: shared delta applied to every dragged item.
      if (!drag_.dragItems_.empty()) {
        for (size_t i = 0; i < drag_.dragItems_.size(); ++i) {
          const Item& it = drag_.dragItems_[i];
          if (it.row < 0 || it.row >= (int)s_.tracks().size()) continue;
          auto& t = s_.tracks()[it.row];
          const double ns = clamp0(quant(drag_.origStarts_[i] + delta));
          if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size())
            t.clips[it.clipIdx].startBeats = ns;
          else if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size())
            t.regions[it.regionIdx].startBeats = ns;
        }
        update();
        playlist_->update();
        return;
      }
    }
    if (drag_.kind == Drag::MoveClip || drag_.kind == Drag::ResizeClip ||
        drag_.kind == Drag::TrimClip) {
      auto& t = s_.tracks()[drag_.row];
      if (drag_.clipIdx >= 0 && drag_.clipIdx < (int)t.clips.size()) {
        auto& c = t.clips[drag_.clipIdx];
        if (drag_.kind == Drag::MoveClip)
          c.startBeats = clamp0(quant(drag_.origStart + delta));
        else if (drag_.kind == Drag::ResizeClip) {
          c.durationBeats = qMax(kMinDur, quant(drag_.origDur + delta));
        } else { // TrimClip: keep right edge, push start/offset into source
          double right = drag_.origStart + drag_.origDur;
          double start = qBound(0.0, quant(drag_.origStart + delta),
                                right - kMinDur);
          double dur = right - start;
          double oldOffset = c.clipOffsetBeats;
          c.startBeats = start;
          c.durationBeats = dur;
          c.clipOffsetBeats = qBound(0.0,
              oldOffset + (start - drag_.origStart), oldOffset + drag_.origDur - dur);
        }
      }
    }
    if (drag_.kind == Drag::MoveRegion || drag_.kind == Drag::ResizeRegion ||
        drag_.kind == Drag::TrimRegion) {
      auto& t = s_.tracks()[drag_.row];
      if (drag_.regionIdx >= 0 && drag_.regionIdx < (int)t.regions.size()) {
        auto& rg = t.regions[drag_.regionIdx];
        if (drag_.kind == Drag::MoveRegion)
          rg.startBeats = clamp0(quant(drag_.origStart + delta));
        else if (drag_.kind == Drag::ResizeRegion)
          rg.durationBeats = qMax(kMinDur, quant(drag_.origDur + delta));
        else { // TrimRegion: left edge only
          double right = drag_.origStart + drag_.origDur;
          double start = qBound(0.0, quant(drag_.origStart + delta),
                                right - kMinDur);
          rg.startBeats = start;
          rg.durationBeats = right - start;
        }
      }
    }
    if (drag_.kind == Drag::Marquee) {
      drag_.lastPt = pt;
      updateMarquee();
    }
    update();
    playlist_->update();
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    Q_UNUSED(e);
    if (drag_.kind == Drag::MoveClip || drag_.kind == Drag::ResizeClip ||
        drag_.kind == Drag::TrimClip || drag_.kind == Drag::MoveRegion ||
        drag_.kind == Drag::ResizeRegion || drag_.kind == Drag::TrimRegion ||
        drag_.kind == Drag::Slice) {
      emit playlist_->edited();
    }
    drag_.kind = Drag::None;
    update();
    playlist_->update();
  }

public:
  void keyPressEvent(QKeyEvent* e) override {
    auto items = selectedItems();
    const int k = e->key();
    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);

    if ((k == Qt::Key_Delete || k == Qt::Key_Backspace) && !items.empty()) {
      // Delete items (regions first so indexes stay valid for clips).
      std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.row != b.row) return b.row < a.row; // topmost first (no matter)
        if (a.regionIdx >= 0 && b.regionIdx >= 0) return a.regionIdx > b.regionIdx;
        if (a.clipIdx >= 0 && b.clipIdx >= 0) return a.clipIdx > b.clipIdx;
        return false;
      });
      for (auto& it : items) {
        if (it.row >= 0 && it.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[it.row];
          if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size())
            s_.removeRegion(t.id, t.regions[it.regionIdx].id);
          else if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size())
            t.clips.erase(t.clips.begin() + it.clipIdx);
        }
      }
      selClip_ = -1; selRegion_ = -1; multi_.clear();
      emit playlist_->edited();
      emit playlist_->selectionChanged();
      update(); return;
    }
    if (ctrl && k == Qt::Key_X && !items.empty()) {
      // Cut: copy the selection to the clipboard, then delete it (same item
      // removal path as Del/Backspace). Regions are removed via the session so
      // ids stay consistent; the copied clipboard stays valid for the next paste.
      clipBoard_.clear();
      double anchor = 1e18;
      for (auto& it : items)
        anchor = qMin(anchor, itemStart(it));
      for (auto& it : items)
        clipBoard_.push_back({it.row, it.clipIdx >= 0, it.clipIdx, it.regionIdx,
                              itemStart(it) - anchor});
      clipAnchor_ = anchor;
      std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.row != b.row) return b.row < a.row;
        if (a.regionIdx >= 0 && b.regionIdx >= 0) return a.regionIdx > b.regionIdx;
        if (a.clipIdx >= 0 && b.clipIdx >= 0) return a.clipIdx > b.clipIdx;
        return false;
      });
      for (auto& it : items) {
        if (it.row >= 0 && it.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[it.row];
          if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size())
            s_.removeRegion(t.id, t.regions[it.regionIdx].id);
          else if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size())
            t.clips.erase(t.clips.begin() + it.clipIdx);
        }
      }
      selClip_ = -1; selRegion_ = -1; multi_.clear();
      emit playlist_->edited();
      emit playlist_->selectionChanged();
      update(); return;
    } else if (ctrl && k == Qt::Key_C && !items.empty()) {
      clipBoard_.clear();
      double anchor = 1e18;
      for (auto& it : items)
        anchor = qMin(anchor, itemStart(it));
      for (auto& it : items)
        clipBoard_.push_back({it.row, it.clipIdx >= 0, it.clipIdx, it.regionIdx,
                              itemStart(it) - anchor});
      clipAnchor_ = anchor;
      return;
    }
    if (ctrl && k == Qt::Key_D && !items.empty()) {
      for (auto& it : items) {
        double ns = itemStart(it) + itemDur(it);
        if (it.clipIdx >= 0 && it.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[it.row];
          if (it.clipIdx < (int)t.clips.size()) {
            auto c = t.clips[it.clipIdx];
            c.id.clear();
            c.startBeats = ns;
            s_.addClip(t.id, std::move(c));
          }
        } else if (it.regionIdx >= 0 && it.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[it.row];
          if (it.regionIdx < (int)t.regions.size())
            duplicateRegionAt(t.id, t.regions[it.regionIdx], ns);
        }
      }
      emit playlist_->edited();
      update(); return;
    }
    if (ctrl && k == Qt::Key_V && !clipBoard_.empty()) {
      double base = playhead_ >= 0 ? playhead_
                   : clipAnchor_ >= 0 ? clipAnchor_ : 0.0;
      for (auto& ci : clipBoard_) {
        double ns = qMax(0.0, base + ci.relStart);
        if (ci.midi && ci.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[ci.row];
          if (ci.regionIdx >= 0 && ci.regionIdx < (int)t.regions.size())
            duplicateRegionAt(t.id, t.regions[ci.regionIdx], ns);
        } else if (ci.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[ci.row];
          if (ci.clipIdx >= 0 && ci.clipIdx < (int)t.clips.size()) {
            auto c = t.clips[ci.clipIdx];
            c.id.clear();
            c.startBeats = ns;
            s_.addClip(t.id, std::move(c));
          }
        }
      }
      emit playlist_->edited();
      update(); return;
    }
    if ((k == Qt::Key_Return || k == Qt::Key_Enter) && selRow_ >= 0) {
      auto& t = s_.tracks()[selRow_];
      if (selRegion_ >= 0 && selRegion_ < (int)t.regions.size())
        emit playlist_->openInPianoRoll(QString::fromStdString(t.id),
                                        QString::fromStdString(t.regions[selRegion_].id));
      else if (selClip_ < 0)
        emit playlist_->openInPianoRoll(QString::fromStdString(t.id), QString());
      return;
    }
    QWidget::keyPressEvent(e);
  }

  std::string duplicateRegionAt(const std::string& trackId,
                                const misuc::project::MidiRegion& src,
                                double newStart) {
    const std::string nid = s_.addRegion(trackId, src.name, newStart,
                                         src.durationBeats);
    if (auto* dst = s_.findRegion(trackId, nid))
      dst->notes = src.notes;
    return nid;
  }

  void wheelEvent(QWheelEvent* e) override {
    if (e->modifiers().testFlag(Qt::ControlModifier)) {
      double delta = e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1;
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
    const QString tid = QString::fromStdString(t.id);
    if ((h.kind == Hit::Clip || h.kind == Hit::ResizeRightClip ||
         h.kind == Hit::TrimLeftClip) && h.clipIdx >= 0) {
      menu.addAction("Rename...");
      menu.addAction("Duplicate");
      menu.addAction("Split at Playhead");
      menu.addAction("Slice Here");
      menu.addAction(t.clips[h.clipIdx].gainDb < -90.f ? "Unmute" : "Mute");
      menu.addAction("Delete");
    } else if ((h.kind == Hit::Region || h.kind == Hit::ResizeRightRegion ||
                h.kind == Hit::TrimLeftRegion) && h.regionIdx >= 0) {
      menu.addAction("Rename...");
      menu.addAction("Duplicate");
      menu.addAction("Slice Here");
      menu.addAction("Open in Piano Roll");
      menu.addAction("Delete");
    } else if (h.kind == Hit::Track) {
      menu.addAction("Rename Track...");
    } else return;

    QAction* a = menu.exec(e->globalPos());
    if (!a) return;
    const QString txt = a->text();
    if (txt == "Rename Track...") {
      bool ok; QString n = QInputDialog::getText(this, "Rename Track", "Name:",
          QLineEdit::Normal, QString::fromStdString(t.name), &ok);
      if (ok && !n.isEmpty()) { t.name = n.toStdString(); emit playlist_->edited(); update(); }
      return;
    }
    if (h.kind == Hit::Track) return;
    const double beat = qMax(0.0, (e->pos().x() - kLabelW) / pxPerBeat_);
    if (txt == "Slice Here") {
      if (h.clipIdx >= 0)
        sliceClip(h.row, h.clipIdx, quant(beat));
      else if (h.regionIdx >= 0)
        sliceRegion(h.row, h.regionIdx, quant(beat));
      emit playlist_->edited(); update(); return;
    }
    if (h.kind == Hit::Clip || h.kind == Hit::ResizeRightClip ||
        h.kind == Hit::TrimLeftClip) {
      auto& c = t.clips[h.clipIdx];
      if (txt == "Rename...") {
        bool ok; QString n = QInputDialog::getText(this, "Rename Clip", "Name:",
            QLineEdit::Normal, clipName(c), &ok);
        if (ok && !n.isEmpty()) { c.name = n.toStdString(); emit playlist_->edited(); update(); }
      } else if (txt == "Duplicate") {
        auto copy = c; copy.id.clear();
        copy.startBeats += copy.durationBeats;
        s_.addClip(t.id, std::move(copy));
        emit playlist_->edited(); update();
      } else if (txt == "Split at Playhead") {
        if (playhead_ > c.startBeats && playhead_ < c.startBeats + c.durationBeats)
          sliceClip(h.row, h.clipIdx, playhead_);
        emit playlist_->edited(); update();
      } else if (txt == "Mute") { c.gainDb = -96.f; emit playlist_->edited(); update(); }
      else if (txt == "Unmute") { c.gainDb = 0.f; emit playlist_->edited(); update(); }
      else if (txt == "Delete") {
        t.clips.erase(t.clips.begin() + h.clipIdx);
        selClip_ = -1;
        emit playlist_->edited(); emit playlist_->selectionChanged(); update();
      }
      return;
    }
    if (h.regionIdx >= 0 && h.regionIdx < (int)t.regions.size()) {
      auto& rg = t.regions[h.regionIdx];
      const QString rid = QString::fromStdString(rg.id);
      if (txt == "Rename...") {
        bool ok; QString n = QInputDialog::getText(this, "Rename Region", "Name:",
            QLineEdit::Normal, QString::fromStdString(rg.name), &ok);
        if (ok && !n.isEmpty()) { rg.name = n.toStdString(); emit playlist_->edited(); update(); }
      } else if (txt == "Duplicate") {
        duplicateRegionAt(t.id, rg, rg.startBeats + rg.durationBeats);
        emit playlist_->edited(); update();
      } else if (txt == "Open in Piano Roll") {
        emit playlist_->openInPianoRoll(tid, rid);
      } else if (txt == "Delete") {
        s_.removeRegion(t.id, rg.id);
        selRegion_ = -1;
        emit playlist_->edited(); emit playlist_->selectionChanged(); update();
      }
    }
  }

  void resizeEvent(QResizeEvent*) override { updateGeometry(); }
};

// ───────────────────────── Playlist (outer) ─────────────────────────
Playlist::Playlist(misuc::project::Session& session, QWidget* parent)
  : QWidget(parent), s_(session) {

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
    b->setIconSize(QSize(16, 16));
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setFixedSize(28, 28);
    b->setShortcut(sc);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
  };

  selBtn_  = mkBtn(icons::IconId::Select, "Select (V)", QKeySequence(Qt::Key_V));
  sliceBtn_= mkBtn(icons::IconId::Slice,  "Slice (C)",  QKeySequence(Qt::Key_C));
  snapBtn_ = mkBtn(icons::IconId::Snap,   "Snap (N)",   QKeySequence(Qt::Key_N));
  addBtn_  = mkBtn(icons::IconId::Add,    "Import Audio", QKeySequence(Qt::CTRL | Qt::Key_I));
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

  connect(selBtn_, &QToolButton::toggled, this,
          [=](bool on) { if (on) { sliceBtn_->setChecked(false); setToolBrush(false); } });
  connect(sliceBtn_, &QToolButton::toggled, this,
          [=](bool on) { if (on) { selBtn_->setChecked(false); } setToolBrush(on); });
  connect(snapBtn_, &QToolButton::toggled, this, &Playlist::setSnap);
  connect(addBtn_,  &QToolButton::clicked, this, &Playlist::importAudioDialog);
  connect(zoom_, &QSlider::valueChanged, this, [this](int v) { applyZoom(v); });

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
void Playlist::setToolBrush(bool slice) {
  grid_->sliceTool_ = slice;
  grid_->setCursor(slice ? Qt::CrossCursor : Qt::ArrowCursor);
}
void Playlist::selectTrack(const QString& id) {
  grid_->selRow_ = grid_->trackRow(id);
  grid_->selClip_ = -1;
  grid_->selRegion_ = -1;
  grid_->multi_.clear();
  grid_->update();
}
void Playlist::clearSelection() {
  grid_->selRow_ = -1; grid_->selClip_ = -1; grid_->selRegion_ = -1;
  grid_->multi_.clear(); grid_->update();
}

void Playlist::attachHints(HintManager* hints) {
  if (!hints)
    return;
  hints->registerTool(selBtn_, QStringLiteral("Tool: Select"),
                      QStringLiteral("Select, move, resize and trim clips and MIDI regions"), QKeySequence(Qt::Key_V).toString(QKeySequence::NativeText), icons::IconId::Select);
  hints->registerTool(sliceBtn_, QStringLiteral("Tool: Slice"),
                      QStringLiteral("Split the clip or region under the pointer at that beat"), QKeySequence(Qt::Key_C).toString(QKeySequence::NativeText), icons::IconId::Slice);
  hints->registerTool(snapBtn_, QStringLiteral("Snap to Grid"),
                      QStringLiteral("Quantize clip/region moves and resizes to the beat grid"), QKeySequence(Qt::Key_N).toString(QKeySequence::NativeText), icons::IconId::Snap, [this] {
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

  // Exact-position hints over the arrangement: the ruler reports the beat
  // under the cursor, clips/regions report their identity and span, and the
  // track header mute/solo cells carry live state.
  const QPointer<Playlist> self(this);
  hints->registerCustom(
      grid_, [self](const QPoint& pos) -> std::optional<Hint> {
        if (self.isNull() || !self->grid_)
          return std::nullopt;
        PlaylistGrid* g = self->grid_;
        const misuc::project::Session& s = g->s_;
        const QPoint p = pos;
        Hint h;
        auto trackRow = [&s](int row) -> const misuc::project::Track& {
          static const misuc::project::Track kEmpty;
          return row >= 0 && row < (int)s.tracks().size() ? s.tracks()[row]
                                                          : kEmpty;
        };

        if (p.y() < kRulerH && p.x() >= kLabelW) {
          double beat = qMax(0.0, double(p.x() - kLabelW) / g->pxPerBeat_);
          if (g->snapOn_) beat = quant(beat);
          const double bar = std::floor(beat / 4.0);
          const double inBar = beat - bar * 4.0;
          h.name = QStringLiteral("Timeline Ruler");
          h.description = QStringLiteral("Click to seek · drag to scrub the "
                                         "playhead");
          h.value = [beat, bar, inBar] {
            return QStringLiteral("beat %1 · bar %2.%3")
                .arg(beat, 0, 'f', 3)
                .arg(int(bar) + 1)
                .arg(int(inBar) + 1);
          };
          return h;
        }
        if (p.y() < kRulerH)
          return std::nullopt;
        if (p.x() < kLabelW || p.x() < 0) {
          const int row = (p.y() - kRulerH) / kRowH;
          const auto& t = trackRow(row);
          h.name = QString::fromStdString(t.name);
          h.description = QStringLiteral("Track lane · double-click to rename");
          h.value = [g, row]() -> QString {
            if (row >= 0 && row < (int)g->s_.tracks().size()) {
              const auto& t = g->s_.tracks()[row];
              return t.muted ? QStringLiteral("muted")
                             : t.solo ? QStringLiteral("solo")
                                      : QStringLiteral("live");
            }
            return QString();
          };
          return h;
        }

        const auto hit = g->hitTest(p);
        if (hit.row < 0)
          return std::nullopt;
        const auto& t = trackRow(hit.row);
        const QString tid = QString::fromStdString(t.name);
        switch (hit.kind) {
          case PlaylistGrid::Hit::Mute:
            h.name = tid + QStringLiteral(" · Mute");
            h.description = QStringLiteral("Silence this track in playback");
            h.value = [g, hit] {
              if (hit.row < 0 || hit.row >= (int)g->s_.tracks().size())
                return QString();
              return g->s_.tracks()[hit.row].muted ? QStringLiteral("muted")
                                                   : QStringLiteral("off");
            };
            return h;
          case PlaylistGrid::Hit::Solo:
            h.name = tid + QStringLiteral(" · Solo");
            h.description = QStringLiteral("Isolate this track; any-solo wins");
            h.value = [g, hit] {
              if (hit.row < 0 || hit.row >= (int)g->s_.tracks().size())
                return QString();
              return g->s_.tracks()[hit.row].solo ? QStringLiteral("solo")
                                                  : QStringLiteral("off");
            };
            return h;
          case PlaylistGrid::Hit::Clip:
          case PlaylistGrid::Hit::TrimLeftClip:
          case PlaylistGrid::Hit::ResizeRightClip:
            if (hit.clipIdx < 0 || hit.clipIdx >= (int)t.clips.size())
              return std::nullopt;
            h.name = QStringLiteral("Clip · %1").arg(clipName(t.clips[hit.clipIdx]));
            h.description = QStringLiteral("Audio region · drag to move, edges "
                                           "to trim/resize");
            h.value = [g, hit]() -> QString {
              const auto& tr = g->s_.tracks()[hit.row];
              const auto& c = tr.clips[hit.clipIdx];
              return QStringLiteral("%1 → %2 beats · %3 dB")
                  .arg(c.startBeats, 0, 'f', 2)
                  .arg(c.startBeats + c.durationBeats, 0, 'f', 2)
                  .arg(c.gainDb, 0, 'f', 1);
            };
            return h;
          case PlaylistGrid::Hit::Region:
          case PlaylistGrid::Hit::TrimLeftRegion:
          case PlaylistGrid::Hit::ResizeRightRegion:
            if (hit.regionIdx < 0 || hit.regionIdx >= (int)t.regions.size())
              return std::nullopt;
            h.name = QStringLiteral("MIDI · %1")
                         .arg(QString::fromStdString(
                             t.regions[hit.regionIdx].name));
            h.description = QStringLiteral("Note region · Enter to open the "
                                           "piano roll");
            h.value = [g, hit]() -> QString {
              const auto& tr = g->s_.tracks()[hit.row];
              const auto& r = tr.regions[hit.regionIdx];
              return QStringLiteral("%1 notes · %2 → %3 beats")
                  .arg(int(r.notes.size()))
                  .arg(r.startBeats, 0, 'f', 2)
                  .arg(r.startBeats + r.durationBeats, 0, 'f', 2);
            };
            return h;
          case PlaylistGrid::Hit::Track:
            h.name = tid;
            h.description = QStringLiteral("Track lane · click to select");
            h.value = [g, hit] {
              if (hit.row < 0 || hit.row >= (int)g->s_.tracks().size())
                return QString();
              return g->s_.tracks()[hit.row].muted ? QStringLiteral("muted")
                                                   : QStringLiteral("live");
            };
            return h;
          default:
            break;
        }
        return std::nullopt;
      });
}
QString Playlist::selectedTrackId() const { return grid_->trackIdAt(grid_->selRow_); }
QString Playlist::selectedRegionId() const {
  if (grid_->selRow_ < 0 || grid_->selRow_ >= (int)s_.tracks().size()) return {};
  const auto& t = s_.tracks()[grid_->selRow_];
  if (grid_->selRegion_ >= 0 && grid_->selRegion_ < (int)t.regions.size())
    return QString::fromStdString(t.regions[grid_->selRegion_].id);
  return {};
}
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
  if (grid_->selClip_ >= 0 && grid_->selClip_ < (int)t.clips.size()) {
    const auto& c = t.clips[grid_->selClip_];
    out.clipIndex = grid_->selClip_;
    out.clipName = clipName(c);
    out.clipSource = QString::fromStdString(c.sourceFile);
    out.startBeats = c.startBeats;
    out.durationBeats = c.durationBeats;
    out.gainDb = c.gainDb;
    out.clipOffsetBeats = c.clipOffsetBeats;
  } else if (grid_->selRegion_ >= 0 && grid_->selRegion_ < (int)t.regions.size()) {
    const auto& rg = t.regions[grid_->selRegion_];
    out.midiRegion = true;
    out.regionId = QString::fromStdString(rg.id);
    out.regionName = QString::fromStdString(rg.name);
    out.startBeats = rg.startBeats;
    out.durationBeats = rg.durationBeats;
    out.noteCount = (int)rg.notes.size();
  } else {
    out.noteCount = (int)t.regions.size()
                        ? 0
                        : (int)t.notes.size();
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
    (void)s_.addClip(t.id, std::move(c));
  }
  emit edited();
  grid_->update();
}

} // namespace misuc::ui