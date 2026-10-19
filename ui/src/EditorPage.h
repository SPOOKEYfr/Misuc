#pragma once

#include <QWidget>
#include <QString>
#include <functional>

namespace misuc::project {
class Session;
}

class QListWidget;
class QSlider;
class QCheckBox;
class QComboBox;

namespace misuc::ui {

class HintManager;
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

  // Select the track by id (from the mixer's strips) and mirror its state.
  void selectTrack(const QString& id, bool notify = true);

  // The active piano roll, so the main window can wire audition + playhead.
  PianoRoll* piano() const { return piano_; }

  // Optional hook to relocate an imported source file before the clip is
  // created (e.g. copy it into the project's audio/ directory). Returns the
  // final path to reference; an empty result aborts the import.
  std::function<QString(const QString&)> importAudioSink;

  // Public entry point for the Add menu (Add → New Track).
  void addNewTrack() { addTrack(); }

  // Registers hints for the editor chrome controls.
  void attachHints(HintManager* hints);

 signals:
  void edited();
  void trackSelected(const QString& id);

 private:
  void rebuildTrackList();
  void addTrack();
  void removeTrack();
  void importAudio();
  void onSelectionChanged(int row);
  void emitEdited() { emit edited(); }

  misuc::project::Session* session_ = nullptr;
  PianoRoll* piano_ = nullptr;
  QListWidget* trackList_ = nullptr;
  QSlider* zoomSlider_ = nullptr;
  QSlider* rowHSlider_ = nullptr;
  QCheckBox* snapChk_ = nullptr;
  QComboBox* snapCombo_ = nullptr;
  QString selectedId_;
  HintManager* hints_ = nullptr;
};

} // namespace misuc::ui