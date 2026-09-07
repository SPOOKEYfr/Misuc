#pragma once

#include <QWidget>
#include <QString>

namespace misuc::project {
class Session;
}

class QListWidget;
class QSlider;
class QCheckBox;

namespace misuc::ui {

class PianoRoll;

// Editor page: track list on the left, piano-roll editor on the right. Tracks
// are added/removed here and every mutation emits edited() so the main window
// pushes an undo snapshot. All changes hit the shared Session directly.
class EditorPage : public QWidget {
  Q_OBJECT
public:
  explicit EditorPage(misuc::project::Session& session, QWidget* parent = nullptr);

  // Repopulate list + repaint piano roll (called after open/undo/redo/new).
  void refresh();

  // The active piano roll, so the main window can wire audition + playhead.
  PianoRoll* piano() const { return piano_; }

signals:
  void edited();

private:
  void rebuildTrackList();
  void addTrack();
  void removeTrack();
  void onSelectionChanged(int row);
  void emitEdited() { emit edited(); }

  misuc::project::Session* session_;
  PianoRoll* piano_ = nullptr;
  QListWidget* trackList_ = nullptr;
  QSlider* zoomSlider_ = nullptr;
  QCheckBox* snapChk_ = nullptr;
  QString selectedId_;
};

} // namespace misuc::ui