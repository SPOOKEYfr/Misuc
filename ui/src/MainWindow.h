#pragma once

#include <QMainWindow>
#include <QString>
#include <optional>

#include <misuc/engine/core/UndoStack.h>
#include <misuc/engine/project/Session.h>

class QAction;
class QTimer;
class QLabel;
class QTabWidget;
class QDockWidget;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class HeaderBar;
class HintManager;
class StatusPanel;
class ProjectPage;
class EditorPage;
class AudioSettingsPage;
class MixerPage;
class ThemePage;
class TransportBar;
class Playlist;
class ChannelRack;
class Browser;
class Inspector;
class WorkspaceManager;

// Application shell: global header toolbar (brand + transport + status),
// a tabbed central workspace (Project / Editor / Audio / Appearance), a
// dockable Mixer panel, a contextual HintBar + status strip, and the File /
// Edit / Workspace / Help menus. Project document semantics are unchanged.
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
  // Test hooks: direct save/load with an explicit path (used by the harness
  // to verify the portable asset-path round-trip and app restore).
  bool writeProject(const QString& path);
  void openProject(const QString& path);

  HeaderBar* headerBar() const { return headerBar_; }

protected:
  void paintEvent(QPaintEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

private:
  void buildMenus();
  void buildPages();
  void buildCommands();
  void poll();
  void feedPlayhead();
  void onSessionEdited();
  void refreshInspector();
  void syncWorkspaceStrip();
  // Help -> About Misuc…
  void showAbout();
  void applyLayoutPreset(const QString& name);
  void persistLayout();
  void restoreLayout();

  void newProject();
  void openDialog();
  bool saveProject();
  bool saveProjectAs();
  void exportAudio();
  void performUndo();
  void performRedo();
  void togglePlayStop();
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

  HeaderBar* headerBar_ = nullptr;
  TransportBar* transport_ = nullptr;
  StatusPanel* status_ = nullptr;
  HintManager* hints_ = nullptr;
  ProjectPage* projectPage_ = nullptr;
  EditorPage* editorPage_ = nullptr;
  AudioSettingsPage* audioSettings_ = nullptr;
  MixerPage* mixerPage_ = nullptr;
  ThemePage* themePage_ = nullptr;
  Playlist* playlist_ = nullptr;
  ChannelRack* channelRack_ = nullptr;
  Browser* browser_ = nullptr;
  Inspector* inspector_ = nullptr;
  WorkspaceManager* wsMan_ = nullptr;
  QTimer* timer_ = nullptr;
  QTabWidget* workspace_ = nullptr;
  QDockWidget* mixerDock_ = nullptr;

  // Headless/testing hook: when set, exportAudio() skips the save dialog and
  // renders directly to this path.
  QString exportPathOverride_;

  // Beat position where recording started (kept in beats so it survives
  // engine-side resampling); used to place the finished clip on the timeline.
  double recordStartBeats_ = 0.0;
  int recordCounter_ = 0;

  QAction* undoCmd_ = nullptr;
  QAction* redoCmd_ = nullptr;
  QAction* snapCmd_ = nullptr;
  QAction* modeCmd_ = nullptr;   // song/pattern
  QAction* metronomeCmd_ = nullptr;
  QString inspectorClipId_;
  int inspectorTrackRow_ = -1;
  bool inspectorOnClip_ = false;
};

} // namespace misuc::ui