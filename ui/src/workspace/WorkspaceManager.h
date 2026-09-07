#pragma once

#include <QObject>
#include <QString>
#include <QHash>
#include <QVector>

class QMainWindow;
class QWidget;
class QDockWidget;
class QTabWidget;

namespace misuc::ui {

class WorkspaceManager;

// Workspace identifiers shared by the header strip, menus, keyboard shortcuts
// and layout presets.
namespace workspaces {
enum class Id {
  Playlist,
  PianoRoll,
  ChannelRack,
  Mixer,
  Browser,
  Inspector,
  Project,
  Audio,
  Appearance,
  Misc,
};
QString name(Id);
} // namespace workspaces

// Owns every workspace page. Pages register either as central tabs
// (Playlist, Piano Roll, Project/Audio/Appearance) or as dockable panels
// (Browser, Inspector, Mixer, Channel Rack). The manager keeps a single
// source of truth for visibility, applies layout presets and persists the
// arrangement (dock state + active tab) across runs.
class WorkspaceManager : public QObject {
  Q_OBJECT
public:
  explicit WorkspaceManager(QMainWindow* win, QTabWidget* tabs);

  void addCentral(workspaces::Id id, QWidget* page);
  void addDock(workspaces::Id id, QWidget* page, Qt::DockWidgetArea area,
               const QString& title);

  QWidget* widget(workspaces::Id id) const;
  bool isVisible(workspaces::Id id) const;
  void showWorkspace(workspaces::Id id, bool reveal);
  void toggleWorkspace(workspaces::Id id);
  void raiseCentral(workspaces::Id id);
  void setCurrentCentral(int index);

  QStringList presetNames() const;
  void applyPreset(const QString& name);
  QString presetName() const;

  void saveLayoutState();
  void restoreLayoutState();
  QMainWindow* mainWindow() const { return win_; }
  QDockWidget* docks(workspaces::Id id) const;

signals:
  void workspaceToggled(workspaces::Id id, bool visible);
  void centralChanged(workspaces::Id id);

private:
  struct Entry {
    workspaces::Id id = workspaces::Id::Misc;
    QString title;
    QWidget* widget = nullptr;
    QDockWidget* dock = nullptr;
    bool central = false;
  };

  Entry* entry(workspaces::Id id);
  const Entry* entry(workspaces::Id id) const;

  QMainWindow* win_ = nullptr;
  QTabWidget* tabs_ = nullptr;
  QVector<Entry*> entries_;
  QHash<workspaces::Id, Entry*> byId_;
  QString preset_;
};

} // namespace misuc::ui