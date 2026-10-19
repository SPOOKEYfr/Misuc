#pragma once

#include <QWidget>

#include <functional>
#include <vector>

#include <misuc/engine/project/Session.h>

#include "PlaylistSelection.h"
#include "branding/Icons.h"

class QTimer;
class QToolButton;
class QSlider;

namespace misuc::ui {

class PlaylistGrid;
class HintManager;

// Metadata of one effect control port as reported by the engine's effect
// catalog: its native range + default for building automation lane scales and
// the display label for the band tag/menu. `index` is the Lv2 control port
// index ("fx:<chain>:<port>" addresses a lane by it).
struct FxPortInfo {
  uint32_t index = 0;
  float min = 0.0f;
  float max = 1.0f;
  float def = 0.0f;
  QString label;
};

// Resolves an effect plugin's control ports by URI — provided by the
// MainWindow from the engine's catalog so the Playlist stays engine-agnostic.
using FxPortsResolver = std::function<std::vector<FxPortInfo>(
    const std::string& uri)>;

// Playlist workspace (master spec §12-20, §71): a real arrangement view bound
// to the Session. Left column = track headers; main area = timeline lanes with
// the ruler on top, real audio clips (coarse waves) and real MIDI regions
// (note-minibars) that mirror the session model, and a violet playhead driven
// by the engine transport. Clips and regions natively support move, resize,
// trim, slice, duplicate, rename, delete, marquee multi-select, copy/paste and
// snap-to-grid; double-clicking a region opens it in the piano roll. Files
// dropped from the browser are imported into the selected track at the drop
// beat (real asset copy + clip). All edits mutate the Session and emit edited().
class Playlist : public QWidget {
  Q_OBJECT
public:
  // Snapshot of the current selection used by the Inspector context panel.
  struct SelectionSummary {
    QString trackId;
    int trackRow = -1;
    int clipIndex = -1; // -1 when the selection is a track/MIDI region
    bool midiRegion = false;
    QString regionId;
    QString regionName;
    bool hasClips = false;
    QString clipName;
    QString clipSource;
    double startBeats = 0;
    double durationBeats = 0;
    float gainDb = 0;
    double clipOffsetBeats = 0;
    int noteCount = 0;
    QString trackName;
    bool trackMuted = false;
    bool trackSolo = false;
    float trackGainDb = 0;
    float trackPan = 0;
  };

  explicit Playlist(misuc::project::Session& session, QWidget* parent = nullptr);

  void refresh();
  void feedPlayhead(bool playing, double beats);
  void setSnap(bool on);
  void setSongMode(bool song);
  void setToolBrush(bool slice); // slice tool on/off
  void setPaintTool(bool paint); // paint (brush) tool on/off
  void selectTrack(const QString& id);
  void clearSelection();

  QString selectedTrackId() const;
  QString selectedRegionId() const;
  int selectedClipRow() const; // -1 when nothing is selected
  bool hasSelection() const;
  SelectionSummary selectionSummary() const;

  // Selection model access for Edit menu / shortcuts.
  void selectAll();
  void deselectAll();
  int selectionCount() const;

  // Real sample insertion (browser drag/drop, import dialog).
  void insertSampleAt(const QString& source, double startBeat, int trackRow);
  void importAudioDialog();

  // Paint brush: get the active clip/region for painting (first selected item).
  // Returns true if there is an active paint source.
  bool getActivePaintSource(std::string& outTrackId,
                            std::string& outClipId,
                            std::string& outRegionId,
                            double& outStartBeats,
                            double& outDurationBeats) const;

  // Registers the icon-only toolbar with the global hint manager so hovering
  // any tool surfaces its hint in the HintBar (§78).
  void attachHints(HintManager* hints);

  // Supplies fx control-port metadata (native range/default + label) used by
  // the automation band to scale and label "fx:<chain>:<port>" lanes, and by
  // the band's menu to enumerate a plugin's automatable ports.
  void setFxPortsResolver(FxPortsResolver resolver);
  std::vector<FxPortInfo> portsForUri(const std::string& uri) const;

  // While an automation point is being dragged on the band, reports the
  // dragging row, lane index and the point's live value. Used by the record
  // sweep to tape a band edit at the transport position (Phase Q).
  bool liveBandDrag(int& row, int& autoIdx, double& value) const;

signals:
  void edited();
  void selectionChanged();
  // Fired when an automation-point drag ends (right before edited()). Carries
  // the row + lane index so the app can flush captured band edits.
  void bandDragFinished(int row, int autoIdx);
  // Always carries the target region id (empty when opening a track without
  // regions), so the piano roll binds to the exact region you clicked.
  void openInPianoRoll(const QString& trackId, const QString& regionId);
  void trackSelected(const QString& id);
  // Ruler click request: park the transport caret at this global beat.
  void seekRequested(double beat);

protected:
  void keyPressEvent(QKeyEvent* event) override;

public:
  void applyZoom(double);

private:

  misuc::project::Session& s_;
  PlaylistGrid* grid_ = nullptr;
  PlaylistSelection selection_;
  QSlider* zoom_ = nullptr;
  QToolButton* selBtn_ = nullptr;
  QToolButton* sliceBtn_ = nullptr;
  QToolButton* paintBtn_ = nullptr;
  QToolButton* snapBtn_ = nullptr;
  QToolButton* addBtn_ = nullptr;
  QToolButton* zoomIcon_ = nullptr;
  FxPortsResolver fxPortsResolver_;
};

} // namespace misuc::ui