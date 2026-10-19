#include "WorkspaceManager.h"

#include <QApplication>
#include <QDockWidget>
#include <QMainWindow>
#include <QScreen>
#include <QSettings>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QVariant>
#include <QtGlobal>
#include <cstdio>

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

namespace {

// Versioned with the QMainWindow state so a stale byte array (different dock
// set / renamed object names) is detected and dropped instead of restoring a
// broken arrangement.
constexpr int kSaveVersion = 2;
constexpr int kCustomSaveDebounceMs = 600;

// One dock's target inside a preset: which edge area it lives in, whether it
// is shown, and its preferred pixel size along the area's split axis.
struct DockTarget {
  workspaces::Id id;
  Qt::DockWidgetArea area;
  bool visible;
  int horizontalSize; // width  (left/right docks)
  int verticalSize;   // height (top/bottom docks)
};

struct PresetSpec {
  const char* name;
  workspaces::Id central;
  std::initializer_list<DockTarget> docks;
};

// The four real layout presets. Each is a genuine arrangement — different
// docks visible, on different edges, with different split sizes — produced by
// one workspace engine (QMainWindow dock nesting + splitters). Arrangement
// differs from Classic by splintered proportions (narrower browser, taller
// inspector, shorter mixer) so switching physically moves the dividers. When
// two docks stack in one edge column, only the first carries the column width.
const PresetSpec kPresets[] = {
    {"Classic",
     workspaces::Id::Playlist,
     {{workspaces::Id::Browser, Qt::LeftDockWidgetArea, true, 250, 0},
      {workspaces::Id::Inspector, Qt::RightDockWidgetArea, true, 260, 0},
      {workspaces::Id::Mixer, Qt::BottomDockWidgetArea, true, 0, 200},
      {workspaces::Id::ChannelRack, Qt::RightDockWidgetArea, false, 0, 0}}},
    {"Arrangement",
     workspaces::Id::Playlist,
     {{workspaces::Id::Browser, Qt::LeftDockWidgetArea, true, 200, 0},
      {workspaces::Id::Inspector, Qt::RightDockWidgetArea, true, 300, 0},
      {workspaces::Id::Mixer, Qt::BottomDockWidgetArea, true, 0, 150},
      {workspaces::Id::ChannelRack, Qt::RightDockWidgetArea, false, 0, 0}}},
    {"Production",
     workspaces::Id::Playlist,
     // Channel Rack joins the right edge above the Inspector (one split
     // column); the Rack carries the column width, browser kept narrow.
     {{workspaces::Id::Browser, Qt::LeftDockWidgetArea, true, 190, 0},
      {workspaces::Id::ChannelRack, Qt::RightDockWidgetArea, true, 210, 0},
      {workspaces::Id::Inspector, Qt::RightDockWidgetArea, true, 0, 0},
      {workspaces::Id::Mixer, Qt::BottomDockWidgetArea, true, 0, 170}}},
    {"Editing",
     workspaces::Id::PianoRoll,
     // Channel Rack + Browser share the left column (Rack sets the width),
     // piano roll dominates the centre for note editing.
     {{workspaces::Id::ChannelRack, Qt::LeftDockWidgetArea, true, 290, 0},
      {workspaces::Id::Browser, Qt::LeftDockWidgetArea, true, 0, 0},
      {workspaces::Id::Inspector, Qt::RightDockWidgetArea, true, 250, 0},
      {workspaces::Id::Mixer, Qt::BottomDockWidgetArea, true, 0, 185}}},
};

const PresetSpec* findSpec(const QString& name) {
  for (const PresetSpec& p : kPresets)
    if (name == QLatin1String(p.name))
      return &p;
  return &kPresets[0];
}

} // namespace

WorkspaceManager::WorkspaceManager(QMainWindow* win, QTabWidget* tabs)
    : win_(win), tabs_(tabs) {
  tabs_->setTabsClosable(false);
  tabs_->setMovable(true);
  tabs_->setDocumentMode(true);
  tabs_->setTabPosition(QTabWidget::North);
  // The stacked pages hint larger minimums (PianoRoll grid, Appearance form)
  // which would pin every dock splitter to its minimum and stop real divider
  // resizing. Bound the central stack so dock layouts can actually breathe.
  tabs_->setMinimumSize(500, 280);

  // Enable real splitter nests between docks on the same edge, so panels can
  // be divided and rearranged freely (FL-style window management).
  win_->setDockNestingEnabled(true);

  // Debounced persistence: once the user finishes dragging/undocking, the
  // resulting arrangement is written (never per-pixel during a drag).
  saveTimer_ = new QTimer(this);
  saveTimer_->setSingleShot(true);
  saveTimer_->setInterval(kCustomSaveDebounceMs);
  connect(saveTimer_, &QTimer::timeout, this,
          &WorkspaceManager::saveLayoutStateNow);
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
    if (!applying_)
      onUserArrangementChanged();
    if (!isVisible(id)) emit workspaceToggled(id, false);
    else emit workspaceToggled(id, true);
  });
  // User floats or re-docks a panel by hand -> the arrangement is now Custom.
  connect(dock, &QDockWidget::topLevelChanged, this,
          [this](bool) { onUserArrangementChanged(); });

  if (tabs_ && !connectedCentral_) {
    connectedCentral_ = true;
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
      if (!applying_)
        onUserArrangementChanged();
    });
  }
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
  QStringList names;
  for (const PresetSpec& p : kPresets)
    names << QLatin1String(p.name);
  return names;
}

QString WorkspaceManager::presetName() const {
  return preset_.isEmpty() ? QStringLiteral("Classic") : preset_;
}

bool WorkspaceManager::isCustom() const {
  return preset_ == QStringLiteral("Custom");
}

void WorkspaceManager::resetWorkspace() {
  preset_ = QStringLiteral("Classic");
  applyPresetSpec(preset_);
  saveLayoutStateNow();
  emit layoutChanged();
}

void WorkspaceManager::applyPreset(const QString& name) {
  preset_ = presetNames().contains(name) ? name
                                         : QStringLiteral("Classic");
  applyPresetSpec(preset_);
  saveLayoutStateNow();
  emit layoutChanged();
}

void WorkspaceManager::applyPresetSpec(const QString& name) {
  const PresetSpec* spec = findSpec(name);
  if (!spec) return;

  const bool wasApplying = applying_;
  applying_ = true;

  // Neutral start: hide every dock so the preset defines the visible set.
  QApplication::processEvents();
  for (auto& e : entries_) {
    if (e->dock && e->dock->isVisible())
      e->dock->setVisible(false);
  }
  QApplication::processEvents();

  // Place each visible dock in its target area. addDockWidget() moves an
  // already-added dock to the area, keeping QMainWindow as the single source
  // of truth (no leaflet shims, no dead screenshots, one workspace engine).
  for (const DockTarget& t : spec->docks) {
    auto* e = entry(t.id);
    if (!e || !e->dock)
      continue;
    win_->addDockWidget(t.area, e->dock);
    if (t.visible)
      e->dock->setVisible(true);
  }
  QApplication::processEvents();

  // Apply split proportions after the arrangement so the dividers land where
  // the preset wants them (this is what makes the four presets differ in
  // geometry, not just in which panels are hidden).
  QList<QDockWidget*> hDocks;
  QList<int> hSizes;
  QList<QDockWidget*> vDocks;
  QList<int> vSizes;
  for (const DockTarget& t : spec->docks) {
    auto* e = entry(t.id);
    if (!e || !e->dock || !t.visible)
      continue;
    if (t.area == Qt::LeftDockWidgetArea || t.area == Qt::RightDockWidgetArea) {
      if (t.horizontalSize > 0) {
        hDocks << e->dock;
        hSizes << t.horizontalSize;
      }
    } else {
      if (t.verticalSize > 0) {
        vDocks << e->dock;
        vSizes << t.verticalSize;
      }
    }
  }
  if (hDocks.size())
    win_->resizeDocks(hDocks, hSizes, Qt::Horizontal);
  if (vDocks.size())
    win_->resizeDocks(vDocks, vSizes, Qt::Vertical);
  QApplication::processEvents();

  if (qEnvironmentVariableIsSet("MISUC_WS_DEBUG")) {
    QTextStream db(stdout);
    db << "[wsdebug] " << name << " winW=" << win_->width() << " |";
    for (auto& e : entries_) {
      if (e->dock) {
        db << " " << e->title.toUtf8().constData() << "=" << e->dock->width()
           << "x" << e->dock->height() << "(min" << e->dock->minimumWidth()
           << "x" << e->dock->minimumHeight() << ")";
      }
    }
    db << "\n";
  }

  // Surface the right-column headline panel so tabbed/split columns show the
  // inspector's rails on top.
  if (auto* top = entry(workspaces::Id::Inspector))
    if (top->dock && top->dock->isVisible())
      top->dock->raise();

  if (spec->central != workspaces::Id::Misc && widget(spec->central))
    tabs_->setCurrentWidget(widget(spec->central));

  applying_ = wasApplying;
}

void WorkspaceManager::onUserArrangementChanged() {
  if (applying_ || tearingDown_)
    return;
  if (preset_ != QStringLiteral("Custom")) {
    preset_ = QStringLiteral("Custom");
    emit layoutChanged();
  }
  scheduleCustomSave();
}

void WorkspaceManager::scheduleCustomSave() {
  if (saveQueued_)
    return;
  saveQueued_ = true;
  saveTimer_->start();
}

void WorkspaceManager::saveLayoutState() {
  saveLayoutStateNow();
}

void WorkspaceManager::settleFloatingDocks() {
  for (Entry* e : entries_) {
    if (e->dock && e->dock->isFloating())
      e->dock->setFloating(false);
  }
}

void WorkspaceManager::setTearingDown() {
  tearingDown_ = true;
}

void WorkspaceManager::saveLayoutStateNow() {
  // Any scheduled auto-save is obsolete: this write IS the authoritative state
  // (also clears the debounce timer so a stale snapshot can never overwrite an
  // explicit save that happened later).
  if (saveQueued_)
    saveTimer_->stop();
  saveQueued_ = false;
  // Qt's restoreState() is unsafe around lingering floating docks, and floats
  // are ephemeral by design: never write state while any panel is floating.
  settleFloatingDocks();
  QSettings s;
  s.beginGroup("ui/workspaces");
  s.setValue("dockState", win_->saveState(kSaveVersion));
  s.setValue("saveVersion", kSaveVersion);
  if (tabs_) s.setValue("currentCentral", tabs_->currentIndex());
  s.setValue("preset", preset_);
  s.endGroup();
}

void WorkspaceManager::restoreLayoutState() {
  // Docks are never persisted while floating, and restoreState() is unsafe if
  // any dock is currently floating when called.
  settleFloatingDocks();
  QSettings s;
  s.beginGroup("ui/workspaces");
  QByteArray state = s.value("dockState").toByteArray();
  const int storedVersion = s.value("saveVersion", -1).toInt();
  const QString stored = s.value("preset", QStringLiteral("Classic")).toString();
  int tabIdx = s.value("currentCentral", 0).toInt();
  s.endGroup();

  const bool hasKnownPreset = presetNames().contains(stored);

  applying_ = true;
  bool ok = storedVersion == kSaveVersion && !state.isEmpty();
  if (ok)
    ok = win_->restoreState(state, kSaveVersion);
  applying_ = false;
  if (qEnvironmentVariableIsSet("MISUC_WS_DEBUG")) {
    QTextStream db(stdout);
    db << "[wsdebug] restore stored=" << stored
       << " bytes=" << state.size() << " verOK="
       << (storedVersion == kSaveVersion) << " restoreOK=" << ok << "\n";
  }

  if (ok) {
    // A saved v2 dockState is authoritative even if it was born from a Custom
    // arrangement; just remember whether the active preset name still applies
    // so the View menu / Appearance page report the truth.
    preset_ = hasKnownPreset ? stored : QStringLiteral("Custom");
    if (tabs_ && tabIdx >= 0 && tabIdx < tabs_->count())
      tabs_->setCurrentIndex(tabIdx);
    // Guard floating panels against a workstation change: if a saved window's
    // frame sits off-screen, pull it back onto the nearest available screen.
    for (auto& e : entries_) {
      if (!e->dock || !e->dock->isFloating())
        continue;
      const QRect saved = e->dock->frameGeometry();
      bool onScreen = false;
      QScreen* best = nullptr;
      int bestArea = 0;
      const auto screens = QApplication::screens();
      for (QScreen* scr : screens) {
        const QRect avail = scr->availableGeometry();
        if (avail.intersects(saved)) {
          onScreen = true;
          break;
        }
        const QRect inter = avail.intersected(saved);
        if (inter.width() * inter.height() > bestArea) {
          bestArea = inter.width() * inter.height();
          best = scr;
        }
      }
      if (!onScreen && best) {
        const QRect avail = best->availableGeometry();
        e->dock->setGeometry(QRect(avail.center() -
                                       QPoint(saved.width() / 2,
                                              saved.height() / 2),
                                   saved.size()));
      }
    }
  } else {
    preset_ = hasKnownPreset ? stored : QStringLiteral("Classic");
    applyPresetSpec(preset_);
  }
  emit layoutChanged();
}

WorkspaceManager::Entry* WorkspaceManager::entry(workspaces::Id id) {
  auto it = byId_.find(id);
  return it != byId_.end() ? it.value() : nullptr;
}
const WorkspaceManager::Entry* WorkspaceManager::entry(workspaces::Id id) const {
  auto it = byId_.constFind(id);
  return it != byId_.constEnd() ? it.value() : nullptr;
}

} // namespace misuc::ui