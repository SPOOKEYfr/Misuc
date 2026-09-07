#pragma once

#include <QMainWindow>
#include <QString>

#include <misuc/engine/core/UndoStack.h>
#include <misuc/engine/project/Session.h>

class QStackedWidget;
class QTimer;
class QMenu;
class QAction;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class TransportBar;
class StatusPanel;
class ProjectPage;
class EditorPage;

// Application shell: menu bar (File / Edit), a project document shared between
// the Project page and the file menu, transport on top, navigation rail, a
// stack of real pages (Project, Audio), and the status strip.
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(misuc::engine::AudioEngine& engine);
  ~MainWindow() override;

protected:
  void paintEvent(QPaintEvent* event) override;
  void closeEvent(QCloseEvent* event) override;

private:
  void buildMenus();
  void buildPages();
  void poll();
  void feedPlayhead();
  void onSessionEdited();

  void newProject();
  void openDialog();
  void openProject(const QString& path);
  bool saveProject();
  bool saveProjectAs();
  bool writeProject(const QString& path);
  void performUndo();
  void performRedo();
  void applyTempoToEngine();
  void rebuildMidiScore();
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
  QTimer* timer_ = nullptr;
  QWidget* navRail_ = nullptr;

  QAction* undoCmd_ = nullptr;
  QAction* redoCmd_ = nullptr;
};

} // namespace misuc::ui