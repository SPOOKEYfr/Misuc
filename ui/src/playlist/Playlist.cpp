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
static constexpr int kAutoBand = 16;     // bottom strip of a lane: automation curve

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

// One automation lane of a track with the given param (null when none).
static const misuc::project::AutomationLane* laneFor(
    const misuc::project::Track& t, const std::string& param) {
  for (const auto& l : t.automation)
    if (l.param == param)
      return &l;
  return nullptr;
}
static int laneIndex(const misuc::project::Track& t, const std::string& param) {
  for (int i = 0; i < (int)t.automation.size(); ++i)
    if (t.automation[i].param == param)
      return i;
  return -1;
}

// Value range of an automation param. gain/volume are linear amplitude
// (pan: L/R position in [-1,+1]); `unity` is the neutral resting value.
struct AutoRange { double vmin = 0.0, vmax = 2.0, unity = 1.0; };
static AutoRange autoRange(const std::string& param) {
  if (param == "pan")
    return {-1.0, 1.0, 0.0};
  if (param == "volume")
    return {0.0, 1.0, 1.0};
  return {0.0, 2.0, 1.0}; // gain
}

// Grams an fx automation lane param ("fx:<chain>:<port>") into its indices.
static bool parseFxLaneParam(const std::string& param, uint32_t& chain,
                             uint32_t& port) {
  if (param.size() < 3 || param.compare(0, 3, "fx:") != 0)
    return false;
  const size_t sep = param.find(':', 3);
  if (sep == std::string::npos || sep == 3 || sep + 1 >= param.size())
    return false;
  auto toU32 = [](const std::string& s, uint32_t& dst) {
    if (s.empty() || s.size() > 9)
      return false;
    for (char c : s)
      if (c < '0' || c > '9')
        return false;
    dst = static_cast<uint32_t>(std::stoul(s));
    return true;
  };
  if (!toU32(param.substr(3, sep - 3), chain))
    return false;
  if (!toU32(param.substr(sep + 1), port))
    return false;
  return true;
}

// ───────────────────── PlaylistGrid ─────────────────────────
class PlaylistGrid : public QWidget {
public:
  misuc::project::Session& s_;
  Playlist* playlist_;
  bool playing_ = false;
  double playhead_ = 0;
  // Scroll navigation targets (the wrapping QScrollArea's bars); null when the
  // grid is not yet inside the scroll area. Arrow / Page / Home keys scroll the
  // arrangement instead of being swallowed, so the Playlist is navigable on
  // both axes from the keyboard.
  QScrollBar* hScroll_ = nullptr;
  QScrollBar* vScroll_ = nullptr;
  void setScrollTargets(QScrollBar* h, QScrollBar* v) {
    hScroll_ = h;
    vScroll_ = v;
  }
  double pxPerBeat_ = 24;
  bool snapOn_ = true;
  bool songMode_ = true;
  bool sliceTool_ = false;
  bool paintTool_ = false;

  int selRow_ = -1;
  int selClip_ = -1;    // deprecated: use selection_ for item selection
  int selRegion_ = -1;  // deprecated: use selection_ for item selection

  // One selectable item; either a clip (clipIdx) OR a region (regionIdx).
  struct Item { int row = -1; int clipIdx = -1; int regionIdx = -1; };
  // multi_ is deprecated; use selection_ for multi-selection
  std::vector<Item> multi_;

  // Phase Q: while an automation point is being dragged, report the live
  // location and current value so a record-armed lane can tape the band edit
  // at the transport position. Returns false when no automation drag is active.
  bool liveBandDrag(int& row, int& autoIdx, double& value) const {
    if (drag_.kind != Drag::MoveAutoPoint)
      return false;
    row = drag_.row;
    autoIdx = drag_.autoIdx;
    value = drag_.liveVal;
    return true;
  }

  // clipboard for copy/paste (ID-based for stability)
  struct ClipItem {
    std::string trackId;
    std::string clipId;
    std::string regionId;
    double relStart = 0;
    bool isMidi() const { return !regionId.empty(); }
  };
  std::vector<ClipItem> clipBoard_;
  double clipAnchor_ = -1;

  // drag state
  struct Drag {
    enum Kind { None, PotentialMove, MoveClip, ResizeClip, TrimClip, MoveRegion,
                ResizeRegion, TrimRegion, Slice, Marquee, SeekHead,
                MoveAutoPoint, Paint };
    Kind kind = None; int row = -1; int clipIdx = -1; int regionIdx = -1;
    double origStart = 0, origDur = 0;
    QPoint pressPt, lastPt; double beat0 = 0;
    int autoIdx = -1; int autoPtIdx = -1;   // automation drag target
    double origAutoTime = 0, origAutoVal = 0;
    // Live value of the automation point currently being dragged (Phase Q: a
    // record-armed lane taps this at the transport position instead of
    // re-sampling the same lane it is mutating).
    double liveVal = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    // Aggregated move set: when the user drags an item that belongs to a
    // multi-selection, every selected item shares the same delta (lockstep).
    struct DragItem {
      Item item;                       // index-based for fast access during drag
      misuc::project::Track* track = nullptr;         // resolved pointers
      misuc::project::AudioClip* clip = nullptr;
      misuc::project::MidiRegion* region = nullptr;
      double origStart = 0;
      double origDur = 0;
    };
    std::vector<DragItem> dragItems_;
  };
  Drag drag_;

  // Paint-tool stroke state: the beat of the last painted bar cell and how
  // many blocks this stroke placed (the whole stroke is ONE edited()/undo).
  static constexpr double kPaintCellBeats = 4.0; // one pattern block = one bar
  double paintLastBeat_ = -1.0;
  int paintPlaced_ = 0;

  // Enhanced paint brush state (Comp12 / #9).
  struct PaintSource {
    std::string trackId;
    std::string clipId;
    std::string regionId;
    double startBeats = 0;
    double durationBeats = 4;
    bool valid() const { return !trackId.empty() && (!clipId.empty() || !regionId.empty()); }
  };
  PaintSource paintSource_;
  bool paintActive_ = false;           // true during a paint stroke (left drag)
  std::vector<double> paintVisitedCells_; // cells painted in current stroke (for dedup)
  // Erase stroke tracking
  bool eraseActive_ = false;
  std::vector<std::pair<std::string, std::string>> erasedItems_; // (trackId, clipId/regionId)

  QHash<QString, QPixmap> waveformPixCache_;
  PlaylistSelection* selection_ = nullptr;

  PlaylistGrid(misuc::project::Session& session, Playlist* playlist, PlaylistSelection* selection, QWidget* parent = nullptr)
    : QWidget(parent), s_(session), playlist_(playlist), selection_(selection) {
    setObjectName(QStringLiteral("playlistGrid"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    // Accept the Browser's DragOnly URLs so a sample pack file can be dropped
    // straight onto the arrangement grid (dropEvent() then places a clip at
    // the snapped beat under the cursor). Without acceptDrops() QWidget::event()
    // silently ignores every incoming drop.
    setAcceptDrops(true);
    setMinimumWidth(kLabelW + 200);
    // paintEvent() fills every pixel with an opaque background; telling Qt to
    // skip the automatic background-erase pass removes the double fill/flash
    // during live resizes.
    setAttribute(Qt::WA_OpaquePaintEvent);
    if (selection_) {
      selection_->setSession(&s_);
      selection_->setChangedFn([this]() { update(); });
    }
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

  // Paint tool: brush the active clip/region/pattern onto the playlist.
  // Uses the current paint source (set from selection or rack pattern).
  // A cell that already holds a clip or region is skipped; the brush
  // respects snap grid and deduplicates within the current stroke.
  void paintBlockAt(int row, double beats) {
    const double cell =
        kPaintCellBeats * std::floor(qMax(0.0, beats) / kPaintCellBeats);

    // Deduplicate within stroke: don't paint the same cell twice
    if (std::find(paintVisitedCells_.begin(), paintVisitedCells_.end(), cell)
        != paintVisitedCells_.end()) {
      return;
    }
    paintVisitedCells_.push_back(cell);
    paintLastBeat_ = cell;

    if (row < 0 || row >= (int)s_.tracks().size())
      return;
    auto& t = s_.tracks()[row];

    // Check if cell is already occupied
    // Skip cells already occupied (paint only into free cells)
    for (auto& r : t.regions)
      if (r.startBeats < cell + kPaintCellBeats &&
          r.startBeats + r.durationBeats > cell) {
        return;
      }
    for (auto& c : t.clips)
      if (c.startBeats < cell + kPaintCellBeats &&
          c.startBeats + c.durationBeats > cell) {
        return;
      }

    bool placed = false;

    // 1. If we have an active paint source (clip or region), duplicate it
    if (paintSource_.valid()) {
      if (!paintSource_.clipId.empty()) {
        // Duplicate audio clip
        for (const auto& tr : s_.tracks()) {
          if (tr.id != paintSource_.trackId) continue;
          for (const auto& c : tr.clips) {
            if (c.id == paintSource_.clipId) {
              misuc::project::AudioClip copy = c;
              copy.id.clear();
              copy.startBeats = cell;
              s_.addClip(t.id, std::move(copy));
              placed = true;
              break;
            }
          }
          break;
        }
      } else if (!paintSource_.regionId.empty()) {
        // Duplicate MIDI region, sized to one paint cell (a bar).
        for (const auto& tr : s_.tracks()) {
          if (tr.id != paintSource_.trackId) continue;
          if (auto* rg = s_.findRegion(tr.id, paintSource_.regionId)) {
            const std::string newId = s_.addRegion(t.id, rg->name, cell,
                                                   kPaintCellBeats);
            if (auto* dst = s_.findRegion(t.id, newId))
              dst->notes = rg->notes;
            placed = true;
          }
          break;
        }
      }
    }

    // 2. Fallback: if no paint source, materialize rack pattern (legacy behavior)
    if (!placed) {
      misuc::project::materializePatternRegion(s_, t.id,
                                               s_.channelRack().activePattern,
                                               cell, kPaintCellBeats);
      placed = true;
    }

    if (placed)
      ++paintPlaced_;
  }

  // Erase tool: remove clips/regions intersecting the given cell.
  void eraseAt(int row, double beats) {
    const double cell =
        kPaintCellBeats * std::floor(qMax(0.0, beats) / kPaintCellBeats);
    if (row < 0 || row >= (int)s_.tracks().size())
      return;
    auto& t = s_.tracks()[row];
    const double cellStart = cell;
    const double cellEnd = cell + kPaintCellBeats;

    // Erase clips intersecting the cell
    for (auto it = t.clips.begin(); it != t.clips.end();) {
      if (it->startBeats < cellEnd && it->startBeats + it->durationBeats > cellStart) {
        erasedItems_.emplace_back(t.id, it->id);
        it = t.clips.erase(it);
      } else {
        ++it;
      }
    }
    // Erase regions intersecting the cell
    for (auto it = t.regions.begin(); it != t.regions.end();) {
      if (it->startBeats < cellEnd && it->startBeats + it->durationBeats > cellStart) {
        erasedItems_.emplace_back(t.id, it->id);
        it = t.regions.erase(it);
      } else {
        ++it;
      }
    }
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
    if (!selection_) return {};
    std::vector<Item> out;
    for (const auto& sit : selection_->items()) {
      // Resolve ID-based item to index-based Item
      for (int row = 0; row < (int)s_.tracks().size(); ++row) {
        if (s_.tracks()[row].id != sit.trackId) continue;
        if (!sit.clipId.empty()) {
          for (int ci = 0; ci < (int)s_.tracks()[row].clips.size(); ++ci) {
            if (s_.tracks()[row].clips[ci].id == sit.clipId) {
              out.push_back({row, ci, -1});
              break;
            }
          }
        } else if (!sit.regionId.empty()) {
          for (int ri = 0; ri < (int)s_.tracks()[row].regions.size(); ++ri) {
            if (s_.tracks()[row].regions[ri].id == sit.regionId) {
              out.push_back({row, -1, ri});
              break;
            }
          }
        }
        break;
      }
    }
    if (out.empty()) {
      Item it = primaryItem();
      if (it.row >= 0) out.push_back(it);
    }
    return out;
  }
  bool isItemSelected(const Item& it) const {
    if (!selection_) return false;
    // Resolve to ID-based item and check selection
    if (it.row < 0 || it.row >= (int)s_.tracks().size()) return false;
    const auto& t = s_.tracks()[it.row];
    PlaylistSelection::Item sit;
    sit.trackId = t.id;
    if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size()) {
      sit.clipId = t.clips[it.clipIdx].id;
    } else if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size()) {
      sit.regionId = t.regions[it.regionIdx].id;
    }
    return selection_->contains(sit);
  }

  // ── hit test ──
  struct Hit {
    enum Kind { None, Track, Clip, Region, ResizeRightClip, TrimLeftClip,
                ResizeRightRegion, TrimLeftRegion, Mute, Solo,
                AutoPoint, AutoBand };
    Kind kind = None; int row = -1; int clipIdx = -1; int regionIdx = -1;
    int autoIdx = -1; int autoPtIdx = -1; // automation lane / point index
  };

  // ── automation band geometry ──
  int autoBandTop(int row) const { return kRulerH + row * kRowH + (kRowH - kAutoBand); }
  int autoBandBottom(int row) const { return autoBandTop(row) + kAutoBand; }
  // The automation param the bottom band edits for a row (default "gain",
  // switched per-row via the band's right-click menu).
  QString bandParam(int row) const {
    auto it = bandParam_.constFind(row);
    return it == bandParam_.cend() ? QStringLiteral("gain") : it.value();
  }
  QHash<int, QString> bandParam_;

  // Value range of an automation param for a row. gain/volume are linear
  // amplitude (pan: L/R position [-1,+1]); fx "fx:<chain>:<port>" params use
  // the plugin's native control range (min/max) with the current parameter
  // value as the resting point (or the plugin default when not overridden).
  AutoRange rangeForRow(int row, const std::string& bp) const {
    if (bp.compare(0, 3, "fx:") != 0)
      return autoRange(bp);
    if (row < 0 || row >= static_cast<int>(s_.tracks().size()))
      return {0.0, 1.0, 0.0};
    const auto& t = s_.tracks()[static_cast<size_t>(row)];
    uint32_t ci = 0, pi = 0;
    if (!parseFxLaneParam(bp, ci, pi) ||
        ci >= static_cast<uint32_t>(t.fx.size()))
      return {0.0, 1.0, 0.0};
    double min = 0.0, max = 1.0, def = 0.0;
    for (const auto& p : playlist_->portsForUri(t.fx[ci].uri)) {
      if (p.index != pi)
        continue;
      min = p.min;
      max = p.max;
      def = p.def;
      break;
    }
    if (max <= min)
      return {0.0, 1.0, 0.0};
    double cur = def;
    for (const auto& p : t.fx[ci].params)
      if (p.index == pi) {
        cur = p.value;
        break;
      }
    return {min, max, std::clamp(cur, min, max)};
  }

  double valueToY(double v, int row) const {
    const AutoRange r = rangeForRow(row, bandParam(row).toStdString());
    const double norm =
        std::clamp((v - r.vmin) / std::max(r.vmax - r.vmin, 1e-9), 0.0, 1.0);
    return autoBandBottom(row) - norm * kAutoBand;
  }
  double yToValue(int y, int row) const {
    const AutoRange r = rangeForRow(row, bandParam(row).toStdString());
    const double norm =
        std::clamp((autoBandBottom(row) - y) / double(kAutoBand), 0.0, 1.0);
    return r.vmin + norm * (r.vmax - r.vmin);
  }

  // Short tag drawn in the band's left square: "PAN"/"VOL"/"GAIN" for the
  // built-ins, the plugin port label for fx lanes.
  QString bandTagLabel(int row, const std::string& bp) const {
    if (bp.compare(0, 3, "fx:") != 0)
      return bp == "pan" ? QStringLiteral("PAN")
             : bp == "volume" ? QStringLiteral("VOL")
                              : QStringLiteral("GAIN");
    if (row < 0 || row >= static_cast<int>(s_.tracks().size()))
      return QStringLiteral("FX");
    const auto& t = s_.tracks()[static_cast<size_t>(row)];
    uint32_t ci = 0, pi = 0;
    if (!parseFxLaneParam(bp, ci, pi) ||
        ci >= static_cast<uint32_t>(t.fx.size()))
      return QStringLiteral("FX");
    for (const auto& p : playlist_->portsForUri(t.fx[ci].uri))
      if (p.index == pi) {
        if (!p.label.isEmpty())
          return p.label;
        break;
      }
    return QStringLiteral("FX%1:%2").arg(ci + 1).arg(pi);
  }
  static int bandTagWidth(const QString& label) {
    return std::clamp(10 + static_cast<int>(label.size()) * 6, 28, 110);
  }
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
    // Automation band sits at the bottom of the lane and wins over the clip/
    // region bodies there, so the curve stays editable even when a region
    // fills the row. Empty sections of the band create new points.
    auto& t = s_.tracks()[row];
    const std::string bp = bandParam(row).toStdString();
    const int ai = laneIndex(t, bp);
    if (pt.y() >= autoBandTop(row)) {
      h.autoIdx = ai;
      if (ai >= 0) {
        const auto& lane = t.automation[ai];
        for (int pi = 0; pi < (int)lane.points.size(); ++pi) {
          const double xp = kLabelW + lane.points[pi].timeBeats * pxPerBeat_;
          if (std::abs(pt.x() - xp) <= 5) {
            h.kind = Hit::AutoPoint; h.autoPtIdx = pi; return h;
          }
        }
      }
      h.kind = Hit::AutoBand; return h;
    }
    // Audio clips first (they can overlap regions but carry a waveform body).
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
    if (selection_) {
      PlaylistSelection::Item sit;
      if (row >= 0 && row < (int)s_.tracks().size()) {
        const auto& t = s_.tracks()[row];
        sit.trackId = t.id;
        if (clipIdx >= 0 && clipIdx < (int)t.clips.size()) {
          sit.clipId = t.clips[clipIdx].id;
        } else if (regionIdx >= 0 && regionIdx < (int)t.regions.size()) {
          sit.regionId = t.regions[regionIdx].id;
        }
      }
      if (!sit.empty()) {
        selection_->setSingle(sit);
      } else {
        selection_->clear();
      }
    }
    emit playlist_->selectionChanged();
  }
  bool inMulti(int row, int clipIdx, int regionIdx) const {
    if (!selection_) return false;
    if (row < 0 || row >= (int)s_.tracks().size()) return false;
    const auto& t = s_.tracks()[row];
    PlaylistSelection::Item sit;
    sit.trackId = t.id;
    if (clipIdx >= 0 && clipIdx < (int)t.clips.size()) {
      sit.clipId = t.clips[clipIdx].id;
    } else if (regionIdx >= 0 && regionIdx < (int)t.regions.size()) {
      sit.regionId = t.regions[regionIdx].id;
    }
    return selection_->contains(sit);
  }
  void updateMarquee() {
    if (!selection_) return;
    std::vector<PlaylistSelection::Item> hits;
    QRectF box = QRectF(drag_.pressPt, drag_.lastPt).normalized();
    for (int row = 0; row < (int)s_.tracks().size(); ++row) {
      const auto& t = s_.tracks()[row];
      const double y0 = kRulerH + row * kRowH;
      if (box.bottom() < y0 || box.top() > y0 + kRowH) continue;
      for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
        const double x0 = kLabelW + t.clips[ci].startBeats * pxPerBeat_;
        const double x1 = x0 + t.clips[ci].durationBeats * pxPerBeat_;
        QRectF r(x0, y0, x1 - x0, kRowH);
        if (r.intersects(box)) {
          hits.push_back({t.id, t.clips[ci].id, {}});
        }
      }
      for (int ri = 0; ri < (int)t.regions.size(); ++ri) {
        const double x0 = kLabelW + t.regions[ri].startBeats * pxPerBeat_;
        const double x1 = x0 + t.regions[ri].durationBeats * pxPerBeat_;
        QRectF r(x0, y0, x1 - x0, kRowH);
        if (r.intersects(box)) {
          hits.push_back({t.id, {}, t.regions[ri].id});
        }
      }
    }
    if (drag_.modifiers & Qt::ControlModifier) {
      for (const auto& it : hits) selection_->add(it);
    } else {
      selection_->selectItems(hits);
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

      // Track number + color indicator
      QString displayName = QString::number(r + 1) + ". " + name;
      p.drawText(10, y + kRowH / 2 + 5, displayName.left(20));

      // Track color indicator (small square next to name)
      if (t.color != 0) {
        QColor trackColor;
        trackColor.setRgba(t.color);
        int cx = 10 + p.fontMetrics().horizontalAdvance(QString::number(r + 1) + ". ");
        p.setPen(Qt::NoPen);
        p.setBrush(trackColor);
        p.drawRect(QRectF(cx + 2, y + 4, 14, kRowH - 8));
      }

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
        bool sel = isItemSelected({r, ci, -1});
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
        p.drawText(body.adjusted(6, 2, -6, -2), Qt::TextSingleLine,
                   p.fontMetrics().elidedText(clipName(c), Qt::ElideRight,
                                              qMax(0, int(body.width()) - 12)));
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
        bool sel = isItemSelected({r, -1, ri});
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
        p.drawText(body.adjusted(6, 1, -6, -1), Qt::TextSingleLine,
                   p.fontMetrics().elidedText(label, Qt::ElideRight,
                                              qMax(0, int(body.width()) - 12)));
        if (sel) {
          p.setPen(Qt::NoPen);
          p.setBrush(theme::kOk);
          double e = 3.0;
          p.drawRect(QRectF(body.left() + 2, body.top() + 8, e, body.height() - 16));
          p.drawRect(QRectF(body.right() - e - 2, body.top() + 8, e, body.height() - 16));
        }
        p.setFont(f);
      }

      // ── automation curve (bottom band). Drawn while a lane with points exists
      // for the band's param; the engine applies the curve per sample during
      // playback and export (gain/volume = amplitude, pan = L/R position).
      const std::string bp = bandParam(r).toStdString();
      const misuc::project::AutomationLane* lane = laneFor(t, bp);
      const AutoRange br = rangeForRow(r, bp);
      if (lane) {
        // Unity baseline (the param's resting value).
        p.setPen(QPen(theme::kGridStrong, 1));
        const double yU = valueToY(br.unity, r);
        p.drawLine(int(kLabelW), int(yU), int(kLabelW + maxEnd() * pxPerBeat_), int(yU));
        p.setPen(QPen(theme::kTextFaint, 1, Qt::DotLine));
        p.drawLine(int(kLabelW), int(autoBandTop(r)), int(kLabelW + maxEnd() * pxPerBeat_), int(autoBandTop(r)));
        // Param tag + record-arm toggle glyph at the band's left edge. The tag
        // shows the param (standing in for the plugin port) and doubles as the
        // arm square: armed shows the danger "R".
        const QString tag = bandTagLabel(r, bp);
        const int tagW = bandTagWidth(tag);
        const bool armed = lane->armed;
        p.setPen(Qt::NoPen);
        p.setBrush(armed ? theme::kDanger : QColor(60, 60, 60, 160));
        p.drawRoundedRect(QRectF(kLabelW + 2, autoBandTop(r) + 2, tagW, 12), 3, 3);
        p.setPen(armed ? Qt::white : theme::kTextFaint);
        p.setFont(QFont(p.font().family(), 7));
        p.drawText(QRectF(kLabelW + 2, autoBandTop(r) + 2, tagW, 12), Qt::AlignCenter,
                   armed ? QStringLiteral("R") : tag);
        if (!lane->points.empty()) {
          // Hold the endpoints flat to the edges, linear between points.
          QPolygonF poly;
          poly << QPointF(double(kLabelW), valueToY(lane->points.front().value, r));
          for (const auto& pt : lane->points)
            poly << QPointF(kLabelW + pt.timeBeats * pxPerBeat_, valueToY(pt.value, r));
          poly << QPointF(double(kLabelW + maxEnd() * pxPerBeat_), valueToY(lane->points.back().value, r));
          p.setPen(QPen(theme::kAccent, 2));
          p.setBrush(Qt::NoBrush);
          p.drawPolyline(poly);
          p.setPen(Qt::NoPen);
          for (int pi = 0; pi < (int)lane->points.size(); ++pi) {
            const double xp = kLabelW + lane->points[pi].timeBeats * pxPerBeat_;
            const bool grabbed = (drag_.kind == Drag::MoveAutoPoint &&
                                  drag_.row == r && drag_.autoPtIdx == pi);
            p.setBrush(grabbed ? theme::kOk : theme::kAccent);
            p.drawEllipse(QPointF(xp, valueToY(lane->points[pi].value, r)), 3.5, 3.5);
          }
        }
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

    // Ghost preview for paint brush
    if (drag_.kind == Drag::Paint && paintTool_ && !eraseActive_) {
      // Show ghost at current mouse position (drag_.lastPt)
      if (drag_.row >= 0 && drag_.row < rows) {
        int y = kRulerH + drag_.row * kRowH;
        double beat = qMax(0.0, (drag_.lastPt.x() - kLabelW) / pxPerBeat_);
        // Apply same snap logic as paintBlockAt
        if (snapOn_ && !QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
          if (QApplication::keyboardModifiers().testFlag(Qt::AltModifier)) {
            beat = std::round(beat * 16.0) / 16.0;
          } else {
            beat = quant(beat);
          }
        }
        const double cell =
            kPaintCellBeats * std::floor(qMax(0.0, beat) / kPaintCellBeats);
        double x0 = kLabelW + cell * pxPerBeat_;
        double w = kPaintCellBeats * pxPerBeat_;
        QRectF ghost(x0, y + 4, w, kRowH - 8);
        // Only show ghost if cell is not occupied
        bool occupied = false;
        if (drag_.row >= 0 && drag_.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[drag_.row];
          for (auto& r : t.regions)
            if (r.startBeats < cell + kPaintCellBeats &&
                r.startBeats + r.durationBeats > cell) { occupied = true; break; }
          for (auto& c : t.clips)
            if (c.startBeats < cell + kPaintCellBeats &&
                c.startBeats + c.durationBeats > cell) { occupied = true; break; }
        }
        if (!occupied && ghost.intersects(QRectF(kLabelW, y, W - kLabelW, kRowH))) {
          QColor ghostColor = theme::kAccent;
          ghostColor.setAlpha(60);
          p.setPen(QPen(ghostColor, 2, Qt::DashLine));
          p.setBrush(QColor(ghostColor.red(), ghostColor.green(), ghostColor.blue(), 30));
          p.drawRoundedRect(ghost, 4, 4);
        }
      }
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
    const std::string origId = r.id;
    // duplicateRegion() may reallocate the region vector that `r` lives in, so
    // snapshot the notes and re-find the original region by id afterwards.
    const auto srcNotes = r.notes;
    const std::string rightId = s_.duplicateRegion(tid, origId);
    misuc::project::MidiRegion* right = s_.findRegion(tid, rightId);
    misuc::project::MidiRegion* left = s_.findRegion(tid, origId);
    if (!right || !left) return;
    right->startBeats = splitGlobal;
    right->durationBeats = origDur - rel;
    std::vector<misuc::project::NoteEvent> leftNotes, rightNotes;
    for (const auto& n : srcNotes) {
      if (n.startBeats >= rel) {
        misuc::project::NoteEvent rn = n;
        rn.startBeats -= rel;
        rightNotes.push_back(rn);
      } else {
        leftNotes.push_back(n);
      }
    }
    left->durationBeats = rel;
    left->notes = std::move(leftNotes);
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
    drag_.liveVal = 0;
    drag_.modifiers = e->modifiers();

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
    if (paintTool_) {
      if (e->pos().x() >= kLabelW && h.row >= 0) {
        // Always clear paint source so paintBlockAt falls through to the
        // rack-pattern materialisation path ("Pattern 1").
        paintSource_.trackId.clear();
        paintSource_.clipId.clear();
        paintSource_.regionId.clear();
        paintVisitedCells_.clear();
        paintLastBeat_ = -1.0;
        paintPlaced_ = 0;

        if (e->button() == Qt::RightButton) {
          // Right-click = erase mode
          drag_.kind = Drag::Paint; // reuse Drag::Paint for erase
          drag_.row = h.row;
          eraseActive_ = true;
          erasedItems_.clear();
          // Erase at this position
          eraseAt(h.row, (e->pos().x() - kLabelW) / pxPerBeat_);
          update();
          return;
        } else {
          // Left-click = paint mode
          drag_.kind = Drag::Paint;
          drag_.row = h.row;
          paintBlockAt(h.row, (e->pos().x() - kLabelW) / pxPerBeat_);
          selRow_ = h.row; selClip_ = -1; selRegion_ = -1;
          if (selection_) selection_->clear();
          emit playlist_->trackSelected(trackIdAt(selRow_));
          emit playlist_->selectionChanged();
          update();
          return;
        }
      }
    }
    if (h.kind == Hit::Track) {
      selRow_ = h.row; selClip_ = -1; selRegion_ = -1;
      if (selection_) selection_->clear();
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
        if (selection_) selection_->clear();
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged();
        emit playlist_->edited(); update(); return;
      }
      if (h.kind == Hit::Region || h.kind == Hit::ResizeRightRegion ||
          h.kind == Hit::TrimLeftRegion) {
        sliceRegion(h.row, h.regionIdx, quant(beat));
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
        if (selection_) selection_->clear();
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged();
        emit playlist_->edited(); update(); return;
      }
      return;
    }

    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);

    if (h.kind == Hit::ResizeRightClip || h.kind == Hit::Clip ||
        h.kind == Hit::TrimLeftClip) {
      // Build selection item
      PlaylistSelection::Item sit;
      if (h.row >= 0 && h.row < (int)s_.tracks().size()) {
        const auto& t = s_.tracks()[h.row];
        sit.trackId = t.id;
        if (h.clipIdx >= 0 && h.clipIdx < (int)t.clips.size()) {
          sit.clipId = t.clips[h.clipIdx].id;
        }
      }

      if (ctrl) {
        if (selection_) selection_->toggle(sit);
        selRow_ = h.row; selClip_ = h.clipIdx; selRegion_ = -1;
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged(); update(); return;
      }
      if (shift && selection_ && selection_->hasAnchor()) {
        if (selection_) selection_->rangeTo(sit, ctrl); // additive if ctrl+shift
        selRow_ = h.row; selClip_ = h.clipIdx; selRegion_ = -1;
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged(); update(); return;
      }
      // Normal click: select only this item (unless already in multi-selection)
      if (selection_ && selection_->contains(sit) && selection_->count() > 1) {
        // Item is part of a multi-selection: keep group for drag
        selRow_ = h.row; selClip_ = h.clipIdx; selRegion_ = -1;
      } else {
        if (selection_) selection_->setSingle(sit);
        selRow_ = h.row; selClip_ = h.clipIdx; selRegion_ = -1;
      }
      emit playlist_->trackSelected(trackIdAt(selRow_));
      // Init drag for potential move/resize/trim
      auto& c = s_.tracks()[h.row].clips[h.clipIdx];
      drag_.origStart = c.startBeats;
      drag_.origDur   = c.durationBeats;
      for (auto& it : selectedItems()) {
        // Resolve to pointers for drag
        misuc::project::Track* track_ = nullptr;
        misuc::project::AudioClip* cl = nullptr;
        misuc::project::MidiRegion* rg = nullptr;
        if (it.row >= 0 && it.row < (int)s_.tracks().size()) {
          track_ = &s_.tracks()[it.row];
          if (it.clipIdx >= 0 && it.clipIdx < (int)track_->clips.size())
            cl = &track_->clips[it.clipIdx];
          else if (it.regionIdx >= 0 && it.regionIdx < (int)track_->regions.size())
            rg = &track_->regions[it.regionIdx];
        }
        double os = itemStart(it);
        double od = itemDur(it);
        drag_.dragItems_.push_back({it, track_, cl, rg, os, od});
      }
      drag_.kind = h.kind == Hit::ResizeRightClip ? Drag::ResizeClip
                  : h.kind == Hit::TrimLeftClip  ? Drag::TrimClip
                                                 : Drag::PotentialMove;
      update(); return;
    }
    if (h.kind == Hit::ResizeRightRegion || h.kind == Hit::Region ||
        h.kind == Hit::TrimLeftRegion) {
      // Build selection item
      PlaylistSelection::Item sit;
      if (h.row >= 0 && h.row < (int)s_.tracks().size()) {
        const auto& t = s_.tracks()[h.row];
        sit.trackId = t.id;
        if (h.regionIdx >= 0 && h.regionIdx < (int)t.regions.size()) {
          sit.regionId = t.regions[h.regionIdx].id;
        }
      }

      if (ctrl) {
        if (selection_) selection_->toggle(sit);
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged(); update(); return;
      }
      if (shift && selection_ && selection_->hasAnchor()) {
        if (selection_) selection_->rangeTo(sit, ctrl);
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
        emit playlist_->trackSelected(trackIdAt(selRow_));
        emit playlist_->selectionChanged(); update(); return;
      }
      if (selection_ && selection_->contains(sit) && selection_->count() > 1) {
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
      } else {
        if (selection_) selection_->setSingle(sit);
        selRow_ = h.row; selClip_ = -1; selRegion_ = h.regionIdx;
      }
      emit playlist_->trackSelected(trackIdAt(selRow_));
      auto& rg = s_.tracks()[h.row].regions[h.regionIdx];
      drag_.origStart = rg.startBeats;
      drag_.origDur   = rg.durationBeats;
      for (auto& it : selectedItems()) {
        misuc::project::Track* track_ = nullptr;
        misuc::project::AudioClip* cl = nullptr;
        misuc::project::MidiRegion* rg2 = nullptr;
        if (it.row >= 0 && it.row < (int)s_.tracks().size()) {
          track_ = &s_.tracks()[it.row];
          if (it.clipIdx >= 0 && it.clipIdx < (int)track_->clips.size())
            cl = &track_->clips[it.clipIdx];
          else if (it.regionIdx >= 0 && it.regionIdx < (int)track_->regions.size())
            rg2 = &track_->regions[it.regionIdx];
        }
        double os = itemStart(it);
        double od = itemDur(it);
        drag_.dragItems_.push_back({it, track_, cl, rg2, os, od});
      }
      drag_.kind = h.kind == Hit::ResizeRightRegion ? Drag::ResizeRegion
                  : h.kind == Hit::TrimLeftRegion ? Drag::TrimRegion
                                                  : Drag::PotentialMove;
      update(); return;
    }
    if (h.kind == Hit::AutoPoint || h.kind == Hit::AutoBand) {
      auto& t = s_.tracks()[h.row];
      const std::string bp = bandParam(h.row).toStdString();
      const int tagW = bandTagWidth(bandTagLabel(h.row, bp));
      const bool leftInArm = e->pos().x() >= kLabelW + 2 &&
                             e->pos().x() <= kLabelW + 2 + tagW;
      if (h.kind == Hit::AutoBand && leftInArm) {
        int ai = laneIndex(t, bp);
        if (ai < 0)
          s_.addAutomationLane(t.id, bp, rangeForRow(h.row, bp).unity);
        if ((ai = laneIndex(t, bp)) < 0)
          return;
        t.automation[ai].armed = !t.automation[ai].armed;
        emit playlist_->edited();
        update();
        playlist_->update();
        return;
      }
      int ai = laneIndex(t, bp);
      if (ai < 0) {
        const std::string tid = t.id;
        const std::string lid =
            s_.addAutomationLane(tid, bp, rangeForRow(h.row, bp).unity);
        ai = laneIndex(t, bp);
        if (ai < 0 || s_.findAutomationLane(tid, lid) == nullptr) return;
      }
      if (ai >= (int)t.automation.size()) return;
      auto& lane = t.automation[ai];
      int pi = h.autoPtIdx;
      if (h.kind == Hit::AutoBand || pi < 0 || pi >= (int)lane.points.size()) {
        double beat = qMax(0.0, (e->pos().x() - kLabelW) / pxPerBeat_);
        if (snapOn_) {
          const double q = quant(beat);
          if (q >= 0.0) beat = q;
        }
        const double val = yToValue(e->pos().y(), h.row);
        misuc::project::AutomationPoint np;
        np.timeBeats = beat; np.value = val;
        auto it = std::lower_bound(lane.points.begin(), lane.points.end(), np,
            [](const misuc::project::AutomationPoint& a,
               const misuc::project::AutomationPoint& b){ return a.timeBeats < b.timeBeats; });
        if (it != lane.points.end() && std::abs(it->timeBeats - beat) < 1e-9) return;
        const int npIdx = (int)(it - lane.points.begin());
        lane.points.insert(it, np);
        pi = npIdx;
      }
      selRow_ = h.row; selClip_ = -1; selRegion_ = -1;
      if (selection_) selection_->clear();
      drag_.row = h.row;
      drag_.autoIdx = ai; drag_.autoPtIdx = pi;
      drag_.origAutoTime = lane.points[pi].timeBeats;
      drag_.origAutoVal = lane.points[pi].value;
      drag_.pressPt = e->pos(); drag_.lastPt = e->pos();
      drag_.kind = Drag::MoveAutoPoint;
      update(); return;
    }

    // Empty lane click: deselect all and start marquee (unless ctrl/shift for additive)
    if (!ctrl && !shift) {
      if (selection_) selection_->clear();
      selRow_ = h.row; selClip_ = -1; selRegion_ = -1;
      emit playlist_->trackSelected(trackIdAt(selRow_));
      emit playlist_->selectionChanged();
    }
    drag_.kind = Drag::Marquee;
    update();
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    QPoint pt = e->pos();
    if (drag_.kind == Drag::None) {
      // Hover affordances: only in the empty area above the first track
      // (playhead line / cue area keeps slice/paint tools crosshair).
      if (pt.y() < kRulerH && pt.x() < kLabelW) {
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

    // Drag threshold: transition from PotentialMove to actual drag kind
    if (drag_.kind == Drag::PotentialMove) {
      int dist = (pt - drag_.pressPt).manhattanLength();
      if (dist < QApplication::startDragDistance()) {
        return; // not a drag yet
      }
      // Promote to actual drag kind based on initial hit
      if (drag_.clipIdx >= 0) {
        drag_.kind = drag_.regionIdx >= 0 ? Drag::MoveRegion : Drag::MoveClip;
      } else if (drag_.regionIdx >= 0) {
        drag_.kind = Drag::MoveRegion;
      }
    }

    double delta = double(pt.x() - drag_.pressPt.x()) / pxPerBeat_;
    drag_.lastPt = pt;

    if (drag_.kind == Drag::Paint) {
      const Qt::KeyboardModifiers kmods = e->modifiers();
      const bool shift = kmods.testFlag(Qt::ShiftModifier);
      const bool alt = kmods.testFlag(Qt::AltModifier);
      const bool ctrl = kmods.testFlag(Qt::ControlModifier);


      // Ctrl takes priority: temporarily switch to selection behavior
      if (ctrl) {
        // Let the selection system handle it (mouseMoveEvent for selection is not implemented here;
        // selection is handled on press/release). For now, just don't paint.
        update();
        playlist_->update();
        return;
      }

      if (eraseActive_) {
        // Right-drag erase
        const double beat = qMax(0.0, (pt.x() - kLabelW) / pxPerBeat_);
        const double cell =
            kPaintCellBeats * std::floor(beat / kPaintCellBeats);
        if (std::abs(cell - paintLastBeat_) > 1e-9) {
          eraseAt(drag_.row, beat);
          paintLastBeat_ = cell;
          update();
          playlist_->update();
        }
        return;
      }

      // Normal paint with modifier handling
      double beat = qMax(0.0, (pt.x() - kLabelW) / pxPerBeat_);
      // Shift = bypass snap, Alt = fine snap (1/16 of beat = 1/64 note)
      if (!shift && snapOn_) {
        if (alt) {
          // Fine snap: 1/16 beat
          beat = std::round(beat * 16.0) / 16.0;
        } else {
          // Normal snap
          beat = quant(beat);
        }
      }
      const double cell =
          kPaintCellBeats * std::floor(beat / kPaintCellBeats);
      for (double b = paintLastBeat_ + kPaintCellBeats;
           b <= cell + 1e-9; b += kPaintCellBeats)
        paintBlockAt(drag_.row, b);
      if (std::abs(cell - paintLastBeat_) > 1e-9) {
        update();
        playlist_->update();
      }
      return;
    }

    // Multi-move: use dragItems_ with resolved pointers
    if (drag_.kind == Drag::MoveClip || drag_.kind == Drag::MoveRegion) {
      if (!drag_.dragItems_.empty()) {
        for (size_t i = 0; i < drag_.dragItems_.size(); ++i) {
          auto& di = drag_.dragItems_[i];
          if (!di.track) continue;
          const double ns = clamp0(quant(di.origStart + delta));
          if (di.clip)
            di.clip->startBeats = ns;
          else if (di.region)
            di.region->startBeats = ns;
        }
        update();
        playlist_->update();
        return;
      }
    }

    if (drag_.kind == Drag::MoveAutoPoint) {
      if (drag_.row >= 0 && drag_.row < (int)s_.tracks().size()) {
        auto& t = s_.tracks()[drag_.row];
        if (drag_.autoIdx >= 0 && drag_.autoIdx < (int)t.automation.size()) {
          auto& lane = t.automation[drag_.autoIdx];
          if (drag_.autoPtIdx >= 0 && drag_.autoPtIdx < (int)lane.points.size()) {
            auto& ppt = lane.points[drag_.autoPtIdx];
            double beat = qMax(0.0, (pt.x() - kLabelW) / pxPerBeat_);
            if (snapOn_) beat = quant(beat);
            double lo = -1e9, hi = 1e9;
            if (drag_.autoPtIdx > 0)
              lo = lane.points[drag_.autoPtIdx - 1].timeBeats + 1e-6;
            if (drag_.autoPtIdx + 1 < (int)lane.points.size())
              hi = lane.points[drag_.autoPtIdx + 1].timeBeats - 1e-6;
            ppt.timeBeats = qBound(lo, beat, hi);
            ppt.value = yToValue(pt.y(), drag_.row);
            drag_.liveVal = ppt.value;
          }
        }
      }
      update();
      playlist_->update();
      return;
    }

    // Multi-resize / trim: apply same delta to all dragItems_
    if (drag_.kind == Drag::ResizeClip || drag_.kind == Drag::TrimClip) {
      for (auto& di : drag_.dragItems_) {
        if (!di.clip) continue;
        if (drag_.kind == Drag::ResizeClip) {
          di.clip->durationBeats = qMax(kMinDur, quant(di.origDur + delta));
        } else { // TrimClip
          double right = di.origStart + di.origDur;
          double start = qBound(0.0, quant(di.origStart + delta), right - kMinDur);
          double dur = right - start;
          double oldOffset = di.clip->clipOffsetBeats;
          di.clip->startBeats = start;
          di.clip->durationBeats = dur;
          di.clip->clipOffsetBeats = qBound(0.0,
              oldOffset + (start - di.origStart), oldOffset + di.origDur - dur);
        }
      }
    }
    if (drag_.kind == Drag::ResizeRegion || drag_.kind == Drag::TrimRegion) {
      for (auto& di : drag_.dragItems_) {
        if (!di.region) continue;
        if (drag_.kind == Drag::ResizeRegion) {
          di.region->durationBeats = qMax(kMinDur, quant(di.origDur + delta));
        } else { // TrimRegion
          double right = di.origStart + di.origDur;
          double start = qBound(0.0, quant(di.origStart + delta), right - kMinDur);
          di.region->startBeats = start;
          di.region->durationBeats = right - start;
        }
      }
    }

    if (drag_.kind == Drag::Marquee) {
      // Throttle marquee updates to avoid excessive scans
      if (std::abs(pt.x() - drag_.lastPt.x()) < 2) {
        update();
        playlist_->update();
        return;
      }
      drag_.lastPt = pt;
      updateMarquee();
    }

    update();
    playlist_->update();
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    Q_UNUSED(e);
    if (drag_.kind == Drag::MoveAutoPoint) {
      emit playlist_->bandDragFinished(drag_.row, drag_.autoIdx);
      drag_.liveVal = 0;
    }
    // Don't emit edited() for PotentialMove (click without drag) or empty Marquee
    if (drag_.kind == Drag::MoveClip || drag_.kind == Drag::ResizeClip ||
        drag_.kind == Drag::TrimClip || drag_.kind == Drag::MoveRegion ||
        drag_.kind == Drag::ResizeRegion || drag_.kind == Drag::TrimRegion ||
        drag_.kind == Drag::Slice || drag_.kind == Drag::MoveAutoPoint) {
      emit playlist_->edited();
    }
    if (drag_.kind == Drag::Marquee) {
      // If marquee resulted in empty selection, deselect all
      if (selection_ && selection_->empty()) {
        selRow_ = -1; selClip_ = -1; selRegion_ = -1;
        emit playlist_->selectionChanged();
      }
    }
    if (drag_.kind == Drag::Paint) {
      if (eraseActive_ && !erasedItems_.empty()) {
        emit playlist_->edited();
        eraseActive_ = false;
        erasedItems_.clear();
      } else if (paintPlaced_ > 0) {
        emit playlist_->edited();
      }
      paintPlaced_ = 0;
      paintSource_.trackId.clear();
      paintSource_.clipId.clear();
      paintSource_.regionId.clear();
      paintVisitedCells_.clear();
    }
    drag_.kind = Drag::None;
    update();
    playlist_->update();
  }

  void mouseDoubleClickEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton || e->pos().y() < kRulerH)
      return;
    const int row = (e->pos().y() - kRulerH) / kRowH;
    if (row < 0 || row >= (int)s_.tracks().size()) return;
    auto& t = s_.tracks()[row];
    const int ai = laneIndex(t, bandParam(row).toStdString());
    if (ai < 0 || e->pos().y() < autoBandTop(row)) return;
    auto& lane = t.automation[ai];
    for (int pi = 0; pi < (int)lane.points.size(); ++pi) {
      const double xp = kLabelW + lane.points[pi].timeBeats * pxPerBeat_;
      if (std::abs(e->pos().x() - xp) <= 5) {
        lane.points.erase(lane.points.begin() + pi);
        emit playlist_->edited();
        update();
        return;
      }
    }
    QWidget::mouseDoubleClickEvent(e);
  }

public:
  void keyPressEvent(QKeyEvent* e) override {
    auto items = selectedItems();
    const int k = e->key();
    const bool ctrl = e->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);

    // Arrow-key navigation scrolls the arrangement on both axes (the wrapper
    // QScrollArea's bars). Ctrl+Up/Down moves the selected patterns/clips to
    // the adjacent track instead.
    if (k == Qt::Key_Up || k == Qt::Key_Down ||
        k == Qt::Key_PageUp || k == Qt::Key_PageDown ||
        k == Qt::Key_Home || k == Qt::Key_End ||
        k == Qt::Key_Left || k == Qt::Key_Right) {
      const int stepRows = k == Qt::Key_PageUp || k == Qt::Key_PageDown
                               ? std::max(1, (vScroll_ && vScroll_->pageStep() > 0)
                                                 ? vScroll_->pageStep() / kRowH
                                                 : 8)
                               : 1;
      const double stepBeats = (k == Qt::Key_PageUp || k == Qt::Key_PageDown)
                                   ? std::max(8.0, maxEnd() / 4.0)
                                   : 1.0;
      if (k == Qt::Key_Up && ctrl) {
        moveSelectionToRow(-1);
        return;
      }
      if (k == Qt::Key_Down && ctrl) {
        moveSelectionToRow(1);
        return;
      }
      if (vScroll_) {
        if (k == Qt::Key_Up)
          vScroll_->setValue(vScroll_->value() - stepRows * kRowH);
        else if (k == Qt::Key_Down)
          vScroll_->setValue(vScroll_->value() + stepRows * kRowH);
        else if (k == Qt::Key_PageUp)
          vScroll_->setValue(vScroll_->value() - vScroll_->pageStep());
        else if (k == Qt::Key_PageDown)
          vScroll_->setValue(vScroll_->value() + vScroll_->pageStep());
        else if (ctrl && k == Qt::Key_Home)
          vScroll_->setValue(vScroll_->minimum());
        else if (ctrl && k == Qt::Key_End)
          vScroll_->setValue(vScroll_->maximum());
      }
      if (hScroll_) {
        if (k == Qt::Key_Left)
          hScroll_->setValue(hScroll_->value() - stepBeats * pxPerBeat_);
        else if (k == Qt::Key_Right)
          hScroll_->setValue(hScroll_->value() + stepBeats * pxPerBeat_);
        else if (k == Qt::Key_Home)
          hScroll_->setValue(hScroll_->minimum());
        else if (k == Qt::Key_End)
          hScroll_->setValue(hScroll_->maximum());
      }
      return;
    }

    // Ctrl+A = Select All
    if (ctrl && k == Qt::Key_A) {
      if (selection_) selection_->selectAll();
      emit playlist_->selectionChanged();
      update(); return;
    }
    // Escape = Deselect All
    if (k == Qt::Key_Escape) {
      if (selection_) selection_->clear();
      selRow_ = -1; selClip_ = -1; selRegion_ = -1;
      emit playlist_->selectionChanged();
      update(); return;
    }

    if ((k == Qt::Key_Delete || k == Qt::Key_Backspace) && !items.empty()) {
      // Delete items by ID (regions first so indexes stay valid for clips).
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
      if (selection_) selection_->clear();
      selClip_ = -1; selRegion_ = -1;
      emit playlist_->edited();
      emit playlist_->selectionChanged();
      update(); return;
    }
    if (ctrl && k == Qt::Key_X && !items.empty()) {
      // Cut: copy to clipboard (ID-based), then delete.
      clipBoard_.clear();
      double anchor = 1e18;
      for (auto& it : items) anchor = qMin(anchor, itemStart(it));
      for (auto& it : items) {
        ClipItem ci; ci.relStart = itemStart(it) - anchor;
        if (it.row >= 0 && it.row < (int)s_.tracks().size()) {
          const auto& t = s_.tracks()[it.row];
          ci.trackId = t.id;
          if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size()) {
            ci.regionId = t.regions[it.regionIdx].id;
          } else if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size()) {
            ci.clipId = t.clips[it.clipIdx].id;
          }
        }
        clipBoard_.push_back(ci);
      }
      clipAnchor_ = anchor;
      // Delete
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
      if (selection_) selection_->clear();
      selClip_ = -1; selRegion_ = -1;
      emit playlist_->edited();
      emit playlist_->selectionChanged();
      update(); return;
    } else if (ctrl && k == Qt::Key_C && !items.empty()) {
      clipBoard_.clear();
      double anchor = 1e18;
      for (auto& it : items) anchor = qMin(anchor, itemStart(it));
      for (auto& it : items) {
        ClipItem ci; ci.relStart = itemStart(it) - anchor;
        if (it.row >= 0 && it.row < (int)s_.tracks().size()) {
          const auto& t = s_.tracks()[it.row];
          ci.trackId = t.id;
          if (it.regionIdx >= 0 && it.regionIdx < (int)t.regions.size()) {
            ci.regionId = t.regions[it.regionIdx].id;
          } else if (it.clipIdx >= 0 && it.clipIdx < (int)t.clips.size()) {
            ci.clipId = t.clips[it.clipIdx].id;
          }
        }
        clipBoard_.push_back(ci);
      }
      clipAnchor_ = anchor;
      return;
    }
    if (ctrl && k == Qt::Key_D && !items.empty()) {
      for (auto& it : items) {
        double ns = itemStart(it) + itemDur(it);
        if (it.clipIdx >= 0 && it.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[it.row];
          if (it.clipIdx < (int)t.clips.size()) {
            auto copy = t.clips[it.clipIdx]; copy.id.clear(); copy.startBeats = ns;
            s_.addClip(t.id, std::move(copy));
          }
        } else if (it.regionIdx >= 0 && it.row < (int)s_.tracks().size()) {
          auto& t = s_.tracks()[it.row];
          if (auto* rg = s_.findRegion(t.id, t.regions[it.regionIdx].id)) {
            duplicateRegionAt(t.id, *rg, ns);
          }
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
        if (ci.isMidi()) {
          if (auto* t = s_.findTrack(ci.trackId)) {
            if (auto* rg = s_.findRegion(t->id, ci.regionId)) {
              duplicateRegionAt(t->id, *rg, ns);
            }
          }
        } else if (!ci.clipId.empty()) {
          if (auto* t = s_.findTrack(ci.trackId)) {
            for (auto& c : t->clips) {
              if (c.id == ci.clipId) {
                auto copy = c; copy.id.clear(); copy.startBeats = ns;
                s_.addClip(t->id, std::move(copy));
                break;
              }
            }
          }
        }
      }
      emit playlist_->edited();
      update(); return;
    }
    if ((k == Qt::Key_Return || k == Qt::Key_Enter) && selRow_ >= 0) {
      auto& t = s_.tracks()[selRow_];
      // Use primary selected region if available
      const auto selItems = selectedItems();
      if (!selItems.empty() && selItems.front().regionIdx >= 0) {
        emit playlist_->openInPianoRoll(QString::fromStdString(t.id),
                                        QString::fromStdString(t.regions[selItems.front().regionIdx].id));
      } else {
        emit playlist_->openInPianoRoll(QString::fromStdString(t.id), QString());
      }
      return;
    }
    QWidget::keyPressEvent(e);
  }

  std::string duplicateRegionAt(const std::string& trackId,
                                const misuc::project::MidiRegion& src,
                                double newStart) {
    // addRegion() may reallocate the region vector that `src` lives in, so
    // copy the notes BEFORE pushing (src.notes would be a dangling read after).
    const auto srcNotes = src.notes;
    const std::string nid = s_.addRegion(trackId, src.name, newStart,
                                         src.durationBeats);
    if (auto* dst = s_.findRegion(trackId, nid))
      dst->notes = srcNotes;
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

  // Moves the selected regions/clips one track up (-1) or down (+1), clamping
  // at the arrangement edges. The move preserves each item's timeline
  // placement; clip/region ids are re-assigned by the session (single undo
  // step, same as cut/paste).
  void moveSelectionToRow(int delta) {
    if (delta == 0) return;
    const int rows = rowCount();
    if (rows < 2) return;
    for (auto& it : selectedItems())
      moveItemToRow(it.row, it.clipIdx, it.regionIdx, delta);
    if (selection_) selection_->clear();
    selClip_ = -1; selRegion_ = -1;
    emit playlist_->edited();
    emit playlist_->selectionChanged();
    update();
    playlist_->update();
  }

  // Moves a single (row, clipIndex, regionIndex) item to an adjacent track.
  void moveItemToRow(int srcRow, int clipIdx, int regionIdx, int delta) {
    const int rows = rowCount();
    if (srcRow < 0 || srcRow >= rows) return;
    const int dstRow = std::clamp(srcRow + delta, 0, rows - 1);
    if (dstRow == srcRow) return;
    auto& src = s_.tracks()[srcRow];
    const std::string srcId = src.id;
    const std::string dstId = s_.tracks()[dstRow].id;
    if (regionIdx >= 0 && regionIdx < (int)src.regions.size()) {
      const auto rg = src.regions[regionIdx];
      s_.removeRegion(srcId, rg.id);
      const std::string nid =
          s_.addRegion(dstId, rg.name, rg.startBeats, rg.durationBeats);
      if (!nid.empty())
        if (auto* dst = s_.findRegion(dstId, nid))
          dst->notes = rg.notes;
    } else if (clipIdx >= 0 && clipIdx < (int)src.clips.size()) {
      const auto cl = src.clips[clipIdx];
      for (size_t ci = 0; ci < src.clips.size(); ++ci)
        if (src.clips[ci].id == cl.id) {
          src.clips.erase(src.clips.begin() + ci);
          break;
        }
      s_.addClip(dstId, cl);
    }
  }

  void dropEvent(QDropEvent* e) override {
    if (!e->mimeData()->hasUrls()) return;
    // External drops (Browser assets) carry no internal drag state, so the
    // target lane comes from the drop position itself, exactly like an
    // internal relocation would.
    int row = drag_.row;
    if (row < 0)
      row = qMax(0, qMin<int>(rowCount() - 1, (e->pos().y() - kRulerH) / kRowH));
    for (auto& url : e->mimeData()->urls()) {
      if (!url.isLocalFile()) continue;
      double beat = double(e->pos().x() - kLabelW) / pxPerBeat_;
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
      menu.addAction("Move to Track Above");
      menu.addAction("Move to Track Below");
      menu.addAction("Delete");
    } else if ((h.kind == Hit::Region || h.kind == Hit::ResizeRightRegion ||
                h.kind == Hit::TrimLeftRegion) && h.regionIdx >= 0) {
      menu.addAction("Rename...");
      menu.addAction("Duplicate");
      menu.addAction("Slice Here");
      menu.addAction("Open in Piano Roll");
      menu.addAction("Move to Track Above");
      menu.addAction("Move to Track Below");
      menu.addAction("Delete");
    } else if (h.kind == Hit::Track) {
      menu.addAction("Add Track");
      if (s_.tracks().size() > 1)
        menu.addAction("Delete Track");
      menu.addSeparator();
      menu.addAction(t.muted ? "Unmute" : "Mute");
      menu.addAction(t.solo ? "Unsolo" : "Solo");
      menu.addSeparator();
      menu.addAction("Rename Track...");
    } else if (h.kind == Hit::AutoBand || h.kind == Hit::AutoPoint) {
      // Choose which parameter the bottom band edits for this row. Picking a
      // param the lane already holds just re-points the band at it. Each
      // automatable parameter is a QAction whose data is the exact lane
      // param string ("gain"/"volume"/"pan", or "fx:<chain>:<port>").
      auto addParam = [&](const QString& title, const QString& param) {
        QAction* act = menu.addAction(title);
        act->setData(param);
        return act;
      };
      addParam(QStringLiteral("Automate: Gain..."), QStringLiteral("gain"));
      addParam(QStringLiteral("Automate: Volume..."), QStringLiteral("volume"));
      addParam(QStringLiteral("Automate: Pan..."), QStringLiteral("pan"));
      // FX control-port automation: one entry per discoverable control port of
      // each insert on this track (engine metadata via the Playlist resolver).
      const auto& ft = s_.tracks()[h.row];
      for (int ci = 0; ci < static_cast<int>(ft.fx.size()); ++ci) {
        if (ft.fx[ci].uri.empty())
          continue;
        const auto ports = playlist_->portsForUri(ft.fx[ci].uri);
        if (ports.empty())
          continue;
        QMenu* fxMenu = menu.addMenu(QStringLiteral("Automate FX: slot %1")
                                          .arg(ci + 1));
        for (const auto& pr : ports) {
          const QString label =
              pr.label.isEmpty() ? QStringLiteral("port %1").arg(pr.index)
                                 : pr.label;
          const QString param =
              QStringLiteral("fx:%1:%2").arg(ci).arg(pr.index);
          QAction* act = fxMenu->addAction(label);
          act->setData(param);
        }
      }
    } else return;

    QAction* a = menu.exec(e->globalPos());
    if (!a) return;
    const QString data = a->data().toString();
    if (!data.isEmpty()) {
      bandParam_.insert(h.row, data);
      update();
      return;
    }
    const QString txt = a->text();
    if (txt == "Add Track") {
      QString newName = QStringLiteral("Track %1").arg(s_.tracks().size() + 1);
      s_.addTrack(newName.toStdString());
      emit playlist_->edited();
      update();
      return;
    }
    if (txt == "Delete Track") {
      if (s_.tracks().size() <= 1) return;
      s_.removeTrack(t.id);
      if (selRow_ >= (int)s_.tracks().size())
        selRow_ = s_.tracks().size() - 1;
      if (selection_) selection_->clear();
      emit playlist_->edited();
      emit playlist_->selectionChanged();
      update();
      return;
    }
    if (txt == "Mute" || txt == "Unmute") {
      t.muted = !t.muted;
      emit playlist_->edited();
      update();
      return;
    }
    if (txt == "Solo" || txt == "Unsolo") {
      t.solo = !t.solo;
      emit playlist_->edited();
      update();
      return;
    }
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
    if (txt == "Move to Track Above") {
      moveItemToRow(h.row, h.clipIdx, h.regionIdx, -1);
      emit playlist_->edited(); emit playlist_->selectionChanged(); update();
      return;
    }
    if (txt == "Move to Track Below") {
      moveItemToRow(h.row, h.clipIdx, h.regionIdx, 1);
      emit playlist_->edited(); emit playlist_->selectionChanged(); update();
      return;
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
  paintBtn_= mkBtn(icons::IconId::Paint,  "Paint (B)",  QKeySequence(Qt::Key_B));
  snapBtn_ = mkBtn(icons::IconId::Snap,   "Snap (N)",   QKeySequence(Qt::Key_N));
  addBtn_  = mkBtn(icons::IconId::Add,    "Import Audio", QKeySequence(Qt::CTRL | Qt::Key_I));
  selBtn_->setObjectName(QStringLiteral("plSelect"));
  sliceBtn_->setObjectName(QStringLiteral("plSlice"));
  paintBtn_->setObjectName(QStringLiteral("plPaint"));
  snapBtn_->setObjectName(QStringLiteral("plSnap"));
  addBtn_->setObjectName(QStringLiteral("plAdd"));
  tbLay->addWidget(selBtn_); tbLay->addWidget(sliceBtn_); tbLay->addWidget(paintBtn_);
  tbLay->addSpacing(8);
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
          [=](bool on) {
            if (on) { sliceBtn_->setChecked(false); paintBtn_->setChecked(false); }
            setToolBrush(false);
            setPaintTool(false);
          });
  connect(sliceBtn_, &QToolButton::toggled, this,
          [=](bool on) {
            if (on) { selBtn_->setChecked(false); paintBtn_->setChecked(false); }
            setToolBrush(on);
            setPaintTool(false);
          });
  connect(paintBtn_, &QToolButton::toggled, this,
          [=](bool on) {
            if (on) { selBtn_->setChecked(false); sliceBtn_->setChecked(false); setToolBrush(false); }
            setPaintTool(on);
          });
  connect(snapBtn_, &QToolButton::toggled, this, &Playlist::setSnap);
  connect(addBtn_,  &QToolButton::clicked, this, &Playlist::importAudioDialog);
  connect(zoom_, &QSlider::valueChanged, this, [this](int v) { applyZoom(v); });
  connect(zoomIcon_, &QToolButton::clicked, this, [this] {
    const double step = (grid_->pxPerBeat_ < 40) ? 1.25 : 1.1;
    const int v = int(std::lround(
        qBound(4.0, grid_->pxPerBeat_ * step, 128.0)));
    zoom_->setValue(v);
  });

  grid_ = new PlaylistGrid(s_, this, &selection_, nullptr);
  auto* sa = new QScrollArea(this);
  sa->setWidgetResizable(true);
  sa->setWidget(grid_);
  grid_->setScrollTargets(sa->horizontalScrollBar(), sa->verticalScrollBar());
  sa->setStyleSheet(QString("background: %1;").arg(theme::kBackground.name()));
  lay->addWidget(sa);
  // Playlist snap defaults ON, matching the PianoRoll and the transport bar's
  // initial magnet state.  The button is checked here (after grid_ exists) so
  // the first toggled→setSnap call doesn't dereference a null grid.
  snapBtn_->setChecked(true);
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
  PlaylistGrid* g = grid_;
  if (!g) return;
  // Only repaint when the visible caret actually moves (or the play state
  // flips). Previously this invalidated the whole grid 30×/s even while idle,
  // which made the tab flicker and burned CPU on a machine with many rows.
  if (g->playing_ == playing && std::abs(beats - g->playhead_) < 1e-9)
    return;
  const double oldBeats = g->playhead_;
  g->playing_ = playing;
  g->playhead_ = beats;
  if (playing && oldBeats >= 0 &&
      std::abs(beats - oldBeats) * g->pxPerBeat_ < 64.0) {
    // Small caret step: repaint just the two caret columns (update() clips to
    // the given rect) — old and new position.
    g->update(QRect(kLabelW + int(oldBeats * g->pxPerBeat_) - 6, 0, 13,
                    g->height()));
    g->update(QRect(kLabelW + int(beats * g->pxPerBeat_) - 6, 0, 13,
                    g->height()));
  } else {
    // Start/stop or a big seek: a full repaint is cheaper than two new columns.
    g->update();
  }
}
void Playlist::setSnap(bool on) {
  if (grid_) grid_->snapOn_ = on;
}
void Playlist::setSongMode(bool song) { grid_->songMode_ = song; }

void Playlist::setFxPortsResolver(FxPortsResolver resolver) {
  fxPortsResolver_ = std::move(resolver);
  if (grid_)
    grid_->update();
}

std::vector<FxPortInfo> Playlist::portsForUri(const std::string& uri) const {
  if (fxPortsResolver_)
    return fxPortsResolver_(uri);
  return {};
}

bool Playlist::liveBandDrag(int& row, int& autoIdx, double& value) const {
  return grid_ && grid_->liveBandDrag(row, autoIdx, value);
}
void Playlist::setToolBrush(bool slice) {
  grid_->sliceTool_ = slice;
  grid_->setCursor(slice ? Qt::CrossCursor : Qt::ArrowCursor);
}
void Playlist::setPaintTool(bool paint) {
  grid_->paintTool_ = paint;
  grid_->setCursor(paint ? Qt::CrossCursor : Qt::ArrowCursor);
}
void Playlist::selectTrack(const QString& id) {
  const int row = grid_->trackRow(id);
  const bool sameRow = row >= 0 && grid_->selRow_ == row;
  grid_->selRow_ = row;
  // Keep clip/region selection when the track is (re)selected from within the
  // playlist itself: the grid emits trackSelected() during a clip/region press
  // and MainWindow round-trips it straight back here. Resetting the item here
  // would silently drop the selection the user just made (moving/duplicating/
  // deleting a selected MIDI region would operate on nothing).
  if (sameRow)
    return;
  grid_->selClip_ = -1;
  grid_->selRegion_ = -1;
  grid_->multi_.clear();
  selection_.clear();
  grid_->update();
}
void Playlist::clearSelection() {
  grid_->selRow_ = -1; grid_->selClip_ = -1; grid_->selRegion_ = -1;
  grid_->multi_.clear();
  selection_.clear();
  grid_->update();
}

void Playlist::selectAll() {
  selection_.selectAll();
  grid_->selRow_ = selection_.items().empty() ? -1 : grid_->trackRow(QString::fromStdString(selection_.items().front().trackId));
  grid_->update();
}

void Playlist::deselectAll() {
  selection_.clear();
  grid_->selRow_ = -1; grid_->selClip_ = -1; grid_->selRegion_ = -1;
  grid_->update();
}

int Playlist::selectionCount() const {
  return selection_.count();
}

void Playlist::attachHints(HintManager* hints) {
  if (!hints)
    return;
  hints->registerTool(selBtn_, QStringLiteral("Tool: Select"),
                      QStringLiteral("Select, move, resize and trim clips and MIDI regions"), QKeySequence(Qt::Key_V).toString(QKeySequence::NativeText), icons::IconId::Select);
  hints->registerTool(sliceBtn_, QStringLiteral("Tool: Slice"),
                      QStringLiteral("Split the clip or region under the pointer at that beat"), QKeySequence(Qt::Key_C).toString(QKeySequence::NativeText), icons::IconId::Slice);
  hints->registerTool(paintBtn_, QStringLiteral("Tool: Paint"),
                      QStringLiteral("Drag to paint selected clip/pattern • Right-drag to erase • Ctrl to select • Shift to free paint • Alt for fine snap"),
                      QKeySequence(Qt::Key_B).toString(QKeySequence::NativeText), icons::IconId::Paint, [this] {
                        return grid_->paintTool_ ? QStringLiteral("on") : QStringLiteral("off");
                      });
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
          if (row >= 0 && row < (int)s.tracks().size()) {
            const QString tid = QString::fromStdString(s.tracks()[row].name);
            const int iconX = kLabelW - 42;
            const int iconY = kRulerH + row * kRowH + (kRowH - 18) / 2;
            if (p.x() >= iconX && p.x() <= iconX + 18 && p.y() >= iconY &&
                p.y() <= iconY + 18) {
              h.name = tid + QStringLiteral(" · Mute");
              h.description =
                  QStringLiteral("Silence this track in playback");
              h.value = [self, row]() -> QString {
                if (self.isNull() || row < 0 ||
                    row >= (int)self->grid_->s_.tracks().size())
                  return QString();
                return self->grid_->s_.tracks()[row].muted
                           ? QStringLiteral("muted")
                           : QStringLiteral("off");
              };
              return h;
            }
            if (p.x() >= iconX + 22 && p.x() <= iconX + 22 + 18 &&
                p.y() >= iconY && p.y() <= iconY + 18) {
              h.name = tid + QStringLiteral(" · Solo");
              h.description = QStringLiteral("Isolate this track; any-solo "
                                             "wins");
              h.value = [self, row]() -> QString {
                if (self.isNull() || row < 0 ||
                    row >= (int)self->grid_->s_.tracks().size())
                  return QString();
                return self->grid_->s_.tracks()[row].solo
                           ? QStringLiteral("solo")
                           : QStringLiteral("off");
              };
              return h;
            }
          }
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
QString Playlist::selectedTrackId() const {
  const auto items = selection_.items();
  if (!items.empty())
    return QString::fromStdString(items.front().trackId);
  return grid_->trackIdAt(grid_->selRow_);
}

QString Playlist::selectedRegionId() const {
  const auto items = selection_.items();
  for (const auto& it : items) {
    if (it.isMidi())
      return QString::fromStdString(it.regionId);
  }
  if (grid_->selRow_ < 0 || grid_->selRow_ >= (int)s_.tracks().size()) return {};
  const auto& t = s_.tracks()[grid_->selRow_];
  if (grid_->selRegion_ >= 0 && grid_->selRegion_ < (int)t.regions.size())
    return QString::fromStdString(t.regions[grid_->selRegion_].id);
  return {};
}

int Playlist::selectedClipRow() const {
  const auto items = selection_.items();
  if (!items.empty()) {
    const auto& it = items.front();
    for (int row = 0; row < (int)s_.tracks().size(); ++row) {
      if (s_.tracks()[row].id == it.trackId)
        return row;
    }
  }
  return grid_->selRow_;
}

bool Playlist::hasSelection() const {
  return selection_.count() > 0 || grid_->selRow_ >= 0;
}

Playlist::SelectionSummary Playlist::selectionSummary() const {
  SelectionSummary out;
  const auto items = selection_.items();
  int row = -1;
  if (!items.empty()) {
    const auto& it = items.front();
    for (int r = 0; r < (int)s_.tracks().size(); ++r) {
      if (s_.tracks()[r].id == it.trackId) {
        row = r;
        break;
      }
    }
  } else {
    row = grid_->selRow_;
  }
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

  // Prefer primary selected item for clip/region details
  if (!items.empty()) {
    const auto& it = items.front();
    if (it.isClip()) {
      for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
        if (t.clips[ci].id == it.clipId) {
          const auto& c = t.clips[ci];
          out.clipIndex = ci;
          out.clipName = clipName(c);
          out.clipSource = QString::fromStdString(c.sourceFile);
          out.startBeats = c.startBeats;
          out.durationBeats = c.durationBeats;
          out.gainDb = c.gainDb;
          out.clipOffsetBeats = c.clipOffsetBeats;
          break;
        }
      }
    } else if (it.isMidi()) {
      for (int ri = 0; ri < (int)t.regions.size(); ++ri) {
        if (t.regions[ri].id == it.regionId) {
          const auto& rg = t.regions[ri];
          out.midiRegion = true;
          out.regionId = QString::fromStdString(rg.id);
          out.regionName = QString::fromStdString(rg.name);
          out.startBeats = rg.startBeats;
          out.durationBeats = rg.durationBeats;
          out.noteCount = (int)rg.notes.size();
          break;
        }
      }
    }
  } else {
    // Fallback to grid state
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
  }
  return out;
}

void Playlist::insertSampleAt(const QString& source, double startBeat, int trackRow) {
  misuc::project::AudioClip c;
  c.sourceFile = source.toStdString();
  c.name = source.section(QLatin1Char('/'), -1).toStdString();
  c.startBeats = startBeat;
  c.durationBeats = 4; // fallback for unreadable/metadata-less sources
  // Derive the clip's real span from the decoded file so an imported sample
  // occupies exactly as much of the arrangement as it sounds (a 2 s file at
  // 120 BPM is a 4-beat clip, not a hard-coded placeholder length).
  misuc::audio::AudioBuffer buf;
  std::string err;
  if (misuc::audio::readAudioFile(source.toStdString(), buf, err) &&
      buf.sampleRate > 0.0 && buf.durationSeconds() > 0.0) {
    const double bpm = s_.tempo() > 0.0 ? s_.tempo() : 120.0;
    c.durationBeats = buf.durationSeconds() * bpm / 60.0;
    c.sampleRate = static_cast<uint32_t>(buf.sampleRate);
    c.channels = static_cast<uint16_t>(buf.channels);
  }
  if (trackRow >= 0 && trackRow < (int)s_.tracks().size()) {
    auto& t = s_.tracks()[trackRow];
    (void)s_.addClip(t.id, std::move(c));
  }
  emit edited();
  grid_->update();
}

bool Playlist::getActivePaintSource(std::string& outTrackId,
                                    std::string& outClipId,
                                    std::string& outRegionId,
                                    double& outStartBeats,
                                    double& outDurationBeats) const {
  const auto items = selection_.items();
  if (items.empty())
    return false;
  const auto& it = items.front();
  outTrackId = it.trackId;
  outClipId = it.clipId;
  outRegionId = it.regionId;
  if (!it.clipId.empty()) {
    for (const auto& t : s_.tracks()) {
      if (t.id != it.trackId) continue;
      for (const auto& c : t.clips) {
        if (c.id == it.clipId) {
          outStartBeats = c.startBeats;
          outDurationBeats = c.durationBeats;
          return true;
        }
      }
    }
  } else if (!it.regionId.empty()) {
    for (const auto& t : s_.tracks()) {
      if (t.id != it.trackId) continue;
      for (const auto& r : t.regions) {
        if (r.id == it.regionId) {
          outStartBeats = r.startBeats;
          outDurationBeats = r.durationBeats;
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace misuc::ui