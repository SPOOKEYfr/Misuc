#pragma once

#include <QObject>
#include <QString>
#include <QHash>
#include <QVector>
#include <QList>

class QMainWindow;
class QWidget;
class QDockWidget;
class QTabWidget;
class QTimer;

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
// (Browser, Inspector, Mixer, Channel Rack). The manager drives a genuine
// QMainWindow dock arrangement: each workspace preset is a real set of
// dock areas + split proportions, docking/undocking/resizing is the native
// QDockWidget behaviour (drag out to float, drag to dock, split via the
// dividers), and the arrangement (splitters, tabs, floating geometry) is
// versioned and persisted. When the user rearranges anything by hand the
// active preset degrades to "Custom" and the new state is kept.
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

  // The four real layout presets ("Classic", "Arrangement", "Production",
  // "Editing"). Selecting one restores that arrangement's dock areas, split
  // proportions, visibility and central tab in one pass.
  QStringList presetNames() const;
  void applyPreset(const QString& name);
  QString presetName() const;
  // True once the user has rearranged docks/tabs by hand (dragged, floated,
  // closed, hidden or switched a central tab) away from the chosen preset.
  bool isCustom() const;
  // Re-apply the stock "Classic" arrangement and forget the custom state.
  void resetWorkspace();

  void saveLayoutState();
  void restoreLayoutState();
  QMainWindow* mainWindow() const { return win_; }
  QDockWidget* docks(workspaces::Id id) const;
  // Re-dock every floating panel back into the main window. Called on close
  // so shutdown never leaves orphan top-level float windows behind.
  void settleFloatingDocks();
  // Marks the workspace engine as shutting down so late visibility/float
  // signals fired while the window is being destroyed are ignored.
  void setTearingDown();
  bool isTearingDown() const { return tearingDown_; }

signals:
  void workspaceToggled(workspaces::Id id, bool visible);
  void centralChanged(workspaces::Id id);
  // Emitted when a layout preset is applied, workspace is reset, or the user
  // arrangement changes (so menus/the Appearance page can re-check).
  void layoutChanged();

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

  // Apply one preset's spec (visibility, areas, split proportions, central).
  void applyPresetSpec(const QString& name);
  void scheduleCustomSave();
  void onUserArrangementChanged();
  void saveLayoutStateNow();

  QMainWindow* win_ = nullptr;
  QTabWidget* tabs_ = nullptr;
  QVector<Entry*> entries_;
  QHash<workspaces::Id, Entry*> byId_;
  QString preset_;
  bool applying_ = false;
  bool tearingDown_ = false;
  QTimer* saveTimer_ = nullptr;
  bool saveQueued_ = false;
  bool connectedCentral_ = false;
};

} // namespace misuc::ui