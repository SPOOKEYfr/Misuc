#pragma once

#include <QMainWindow>
#include <QString>
#include <optional>

#include <misuc/engine/core/UndoStack.h>
#include <misuc/engine/project/Session.h>

class QStackedWidget;
class QTimer;
class QMenu;
class QAction;
class QLabel;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class TransportBar;
class StatusPanel;
class ProjectPage;
class EditorPage;
class AudioSettingsPage;
class MixerPage;
class ThemePage;

// Application shell: menu bar (File / Edit), a project document shared between
// the Project page and the file menu, transport on top, navigation rail, a
// stack of real pages (Project, Audio), and the status strip.
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(misuc::engine::AudioEngine& engine);
  ~MainWindow() override;

  // Document access for embedders / tests. Mutations should be followed by
  // sessionEdited() so the engine and undo stack pick up the change.
  misuc::project::Session& session() noexcept { return session_; }
  void sessionEdited() { onSessionEdited(); }
  // Test hook: skip the save dialog and render straight to this path.
  void setExportPathOverride(const QString& path) { exportPathOverride_ = path; }

protected:
  void paintEvent(QPaintEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

private:
  void buildMenus();
  void buildPages();
  void poll();
  void feedPlayhead();
  void onSessionEdited();
  // Repaint the nav-rail brand mark from the current theme accent.
  void applyBranding();

  void newProject();
  void openDialog();
  void openProject(const QString& path);
  bool saveProject();
  bool saveProjectAs();
  bool writeProject(const QString& path);
  void exportAudio();
  void performUndo();
  void performRedo();
  void applyTempoToEngine();
  void applyLoopToEngine(misuc::project::Session& s);
  void rebuildMidiScore();
  void rebuildClipScore();
  void onRecordToggled(bool on);
  // Relocate an imported or recorded audio file into the project's audio/
  // directory when the project has a home; otherwise keep the source path.
  QString stageAudioFile(const QString& src) const;
  void updateWindowTitle();

  // Ask about unsaved changes. Returns false when the user cancelled.
  bool confirmDiscard();

  misuc::engine::AudioEngine& engine_;
  misuc::project::Session session_;
  misuc::core::UndoStack<misuc::project::Session> undo_;
  QString projectPath_;
  bool dirty_ = false;

  QStackedWidget* pages_ = nullptr;
  TransportBar* transport_ = nullptr;
  StatusPanel* status_ = nullptr;
  ProjectPage* projectPage_ = nullptr;
  EditorPage* editorPage_ = nullptr;
  AudioSettingsPage* audioSettings_ = nullptr;
  MixerPage* mixerPage_ = nullptr;
  ThemePage* themePage_ = nullptr;
  QTimer* timer_ = nullptr;
  QWidget* navRail_ = nullptr;
  QLabel* navLogo_ = nullptr;

  // Headless/testing hook: when set, exportAudio() skips the save dialog and
  // renders directly to this path.
  QString exportPathOverride_;

  // Beat position where recording started (kept in beats so it survives
  // engine-side resampling); used to place the finished clip on the timeline.
  double recordStartBeats_ = 0.0;
  int recordCounter_ = 0;

  QAction* undoCmd_ = nullptr;
  QAction* redoCmd_ = nullptr;
};

} // namespace misuc::ui