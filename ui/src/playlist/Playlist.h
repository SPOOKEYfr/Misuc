#pragma once

#include <QWidget>

#include <misuc/engine/project/Session.h>

#include "branding/Icons.h"

class QTimer;
class QToolButton;
class QSlider;

namespace misuc::ui {

class PlaylistGrid;
class HintManager;

// Playlist workspace (master spec §12-20, §71): a real arrangement view bound
// to the Session. Left column = track headers; main area = timeline lanes with
// the ruler on top, real audio clips rendered per type (coarse waves for real
// audio, note-minibars for MIDI regions), and a violet playhead driven by the
// engine transport. Edits (move, resize-right, trim-left, duplicate, split,
// delete, mute, rename, snap-to-grid) mutate the Session and emit edited(),
// exactly like the piano roll. Files dropped from the browser are imported
// into the selected track at the drop beat (real asset copy + clip).
class Playlist : public QWidget {
  Q_OBJECT
public:
  // Snapshot of the current selection used by the Inspector context panel.
  struct SelectionSummary {
    QString trackId;
    int trackRow = -1;
    int clipIndex = -1; // -1 when the selection is a track/MIDI region
    bool midiRegion = false;
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
  void selectTrack(const QString& id);
  void clearSelection();

  QString selectedTrackId() const;
  int selectedClipRow() const; // -1 when nothing is selected
  bool hasSelection() const;
  SelectionSummary selectionSummary() const;

  // Real sample insertion (browser drag/drop, import dialog).
  void insertSampleAt(const QString& source, double startBeat, int trackRow);
  void importAudioDialog();

  // Registers the icon-only toolbar with the global hint manager so hovering
  // any tool surfaces its hint in the HintBar (§78).
  void attachHints(HintManager* hints);

signals:
  void edited();
  void selectionChanged();
  void openInPianoRoll(const QString& trackId);
  void trackSelected(const QString& id);

protected:
  void keyPressEvent(QKeyEvent* event) override;

private:
  void applyZoom(double);

  misuc::project::Session& s_;
  PlaylistGrid* grid_ = nullptr;
  QSlider* zoom_ = nullptr;
  QToolButton* selBtn_ = nullptr;
  QToolButton* sliceBtn_ = nullptr;
  QToolButton* snapBtn_ = nullptr;
  QToolButton* addBtn_ = nullptr;
  QToolButton* zoomIcon_ = nullptr;
};

} // namespace misuc::ui