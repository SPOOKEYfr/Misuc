#include "WorkspaceManager.h"

#include <QDockWidget>
#include <QMainWindow>
#include <QSettings>
#include <QTabWidget>
#include <QVariant>
#include <QtGlobal>

#include "Theme.h"
#include "branding/Icons.h"

namespace misuc::ui {

namespace workspaces {
QString name(Id id) {
  switch (id) {
    case Id::Playlist:    return QStringLiteral("Playlist");
    case Id::PianoRoll:   return QStringLiteral("Piano Roll");
    case Id::ChannelRack: return QStringLiteral("Channel Rack");
    case Id::Mixer:       return QStringLiteral("Mixer");
    case Id::Browser:     return QStringLiteral("Browser");
    case Id::Inspector:   return QStringLiteral("Inspector");
    case Id::Project:     return QStringLiteral("Project");
    case Id::Audio:       return QStringLiteral("Audio");
    case Id::Appearance:  return QStringLiteral("Appearance");
    case Id::Misc:        return QStringLiteral("Other");
  }
  return {};
}
} // namespace workspaces

WorkspaceManager::WorkspaceManager(QMainWindow* win, QTabWidget* tabs)
    : win_(win), tabs_(tabs) {
  tabs_->setTabsClosable(false);
  tabs_->setMovable(true);
  tabs_->setDocumentMode(true);
  tabs_->setTabPosition(QTabWidget::North);
}

void WorkspaceManager::addCentral(workspaces::Id id, QWidget* page) {
  if (entry(id)) return;
  auto* e = new Entry;
  e->id = id;
  e->title = workspaces::name(id);
  e->widget = page;
  e->central = true;
  entries_.push_back(e);
  byId_.insert(id, e);
  tabs_->addTab(page, workspaces::name(id));
  tabs_->setTabIcon(tabs_->indexOf(page), icons::make(
      id == workspaces::Id::Playlist   ? icons::IconId::Playlist
      : id == workspaces::Id::PianoRoll? icons::IconId::PianoRoll
      : icons::IconId::Settings, theme::kTextDim, 16));
}

void WorkspaceManager::addDock(workspaces::Id id, QWidget* page,
                               Qt::DockWidgetArea area, const QString& title) {
  if (entry(id)) return;
  auto* dock = new QDockWidget(page ? title : workspaces::name(id), win_);
  dock->setObjectName(QStringLiteral("dock_") + workspaces::name(id).toLower().replace(' ', '_'));
  dock->setFeatures(QDockWidget::DockWidgetMovable |
                    QDockWidget::DockWidgetFloatable |
                    QDockWidget::DockWidgetClosable);
  dock->setWidget(page);
  dock->setMinimumWidth(180);

  auto* e = new Entry;
  e->id = id;
  e->title = page ? title : workspaces::name(id);
  e->widget = page;
  e->dock = dock;
  e->central = false;
  entries_.push_back(e);
  byId_.insert(id, e);

  win_->addDockWidget(area, dock);

  connect(dock, &QDockWidget::visibilityChanged, this, [this, id](bool v) {
    if (!isVisible(id)) emit workspaceToggled(id, false);
    else emit workspaceToggled(id, true);
  });
}

QDockWidget* WorkspaceManager::docks(workspaces::Id id) const {
  auto e = entry(id);
  return e ? e->dock : nullptr;
}

QWidget* WorkspaceManager::widget(workspaces::Id id) const {
  auto it = byId_.constFind(id);
  return it != byId_.constEnd() ? it.value()->widget : nullptr;
}

bool WorkspaceManager::isVisible(workspaces::Id id) const {
  auto e = entry(id);
  if (!e) return false;
  if (e->dock) return e->dock->isVisible();
  // central tabs are always present; "visible" means it is the current tab
  return tabs_ && tabs_->currentWidget() == e->widget;
}

void WorkspaceManager::showWorkspace(workspaces::Id id, bool reveal) {
  auto* e = const_cast<Entry*>(entry(id));
  if (!e) return;
  if (e->dock) {
    e->dock->setVisible(reveal);
    if (reveal) e->dock->raise();
  } else if (reveal) {
    tabs_->setCurrentWidget(e->widget);
  }
  emit workspaceToggled(id, reveal);
}

void WorkspaceManager::toggleWorkspace(workspaces::Id id) {
  showWorkspace(id, !isVisible(id));
}

void WorkspaceManager::raiseCentral(workspaces::Id id) {
  showWorkspace(id, true);
}

void WorkspaceManager::setCurrentCentral(int index) {
  if (!tabs_) return;
  tabs_->setCurrentIndex(index);
  if (auto* w = tabs_->currentWidget()) {
    auto it = byId_.constFind(workspaces::Id::Misc);
    for (int i = 0; i < entries_.size(); ++i) {
      if (entries_[i]->central && entries_[i]->widget == w) {
        emit centralChanged(entries_[i]->id);
        break;
      }
    }
  }
}

QStringList WorkspaceManager::presetNames() const {
  return {QStringLiteral("Default"),   QStringLiteral("Playlist"),
          QStringLiteral("Piano Roll"), QStringLiteral("Channel Rack"),
          QStringLiteral("Mixing"),    QStringLiteral("Recording")};
}

QString WorkspaceManager::presetName() const {
  return preset_.isEmpty() ? QStringLiteral("Default") : preset_;
}

void WorkspaceManager::applyPreset(const QString& name) {
  preset_ = name;
  namespace ws = workspaces;

  // neutral visible set: all docks hidden, central defaults to Playlist
  for (auto& e : entries_) {
    if (e->dock) e->dock->setVisible(false);
  }

  ws::Id central = ws::Id::Playlist;
  bool withRack = false;
  bool withBrowser = true, withInspector = true, withMixer = true;

  if (name == "Piano Roll")    { central = ws::Id::PianoRoll; }
  if (name == "Channel Rack")  { central = ws::Id::Playlist;  withRack = true; withBrowser = false; }
  if (name == "Mixing")        { central = ws::Id::Playlist;  withBrowser = false; withInspector = false; withMixer = true; }
  if (name == "Recording")     { central = ws::Id::PianoRoll; withRack = true; }
  // Default / Playlist: browser left, inspector right, mixer bottom

  if (auto* e = entry(ws::Id::Browser))  if (e->dock && withBrowser)  e->dock->setVisible(true);
  if (auto* e = entry(ws::Id::Inspector)) if (e->dock && withInspector) e->dock->setVisible(true);
  if (auto* e = entry(ws::Id::Mixer))     if (e->dock && withMixer)    e->dock->setVisible(true);
  if (auto* e = entry(ws::Id::ChannelRack)) if (e->dock && withRack)   e->dock->setVisible(true);

  if (central != ws::Id::Misc && widget(central))
    tabs_->setCurrentWidget(widget(central));
  saveLayoutState();
}

WorkspaceManager::Entry* WorkspaceManager::entry(workspaces::Id id) {
  auto it = byId_.find(id);
  return it != byId_.end() ? it.value() : nullptr;
}
const WorkspaceManager::Entry* WorkspaceManager::entry(workspaces::Id id) const {
  auto it = byId_.constFind(id);
  return it != byId_.constEnd() ? it.value() : nullptr;
}

void WorkspaceManager::saveLayoutState() {
  QSettings s;
  s.beginGroup("ui/workspaces");
  s.setValue("dockState", win_->saveState(1));
  if (tabs_) s.setValue("currentCentral", tabs_->currentIndex());
  s.setValue("preset", preset_);
  s.endGroup();
}

void WorkspaceManager::restoreLayoutState() {
  QSettings s;
  s.beginGroup("ui/workspaces");
  QByteArray state = s.value("dockState").toByteArray();
  bool ok = !state.isEmpty() && win_->restoreState(state, 1);
  int tabIdx = s.value("currentCentral", 0).toInt();
  preset_ = s.value("preset", QStringLiteral("Default")).toString();
  s.endGroup();

  if (ok && tabs_ && tabIdx >= 0 && tabIdx < tabs_->count())
    tabs_->setCurrentIndex(tabIdx);
  else
    applyPreset(preset_);
}

} // namespace misuc::ui