#include "MainWindow.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <QApplication>
#include <QCoreApplication>
#include <QAbstractButton>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QShortcut>
#include <QStringList>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/ClipScore.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/core/ProjectFile.h>
#include "export/ExportDialog.h"
#include "RecordSetupDialog.h"

#include "AudioSettingsPage.h"
#include "EditorPage.h"
#include "HeaderBar.h"
#include "HintSystem.h"
#include "ProjectPage.h"
#include "StatusPanel.h"
#include "Theme.h"
#include "TransportBar.h"
#include "PianoRoll.h"
#include "branding/AboutDialog.h"
#include "branding/Logo.h"
#include "mixer/MixerPage.h"
#include "theme/ThemeEngine.h"
#include "theme/ThemePage.h"
#include "playlist/Playlist.h"
#include "channel/ChannelRack.h"
#include "browser/Browser.h"
#include "inspector/Inspector.h"
#include "project/DemoProject.h"
#include "workspace/WorkspaceManager.h"
#include "commands/MisucCommandRegistry.h"
#include <misuc/engine/flp/FlpReader.h>
#include <misuc/engine/flp/FlpImport.h>

namespace misuc::ui {

namespace {
const QLatin1String kSettingsOrg("misuc");
const QLatin1String kSettingsApp("misuc");
const QLatin1String kAutosaveIntervalKey("autosave/intervalSeconds");
const QLatin1String kRecentProjectsKey("recent/projects");

// Persisted autosave interval in seconds (0 = disabled); 90 s default keeps
// recovery timely while avoiding constant disk traffic. Clamped to a sane
// range regardless of what is stored in the settings file.
int autosaveIntervalFromSettings() {
  QSettings settings(kSettingsOrg, kSettingsApp);
  const int secs =
      settings.value(kAutosaveIntervalKey, 90).toInt();
  return std::clamp(secs, 0, 24 * 3600);
}

// Header strip ids <-> workspace ids (single mapping used by the strip and
// the workspace manager).
QString wsStripId(workspaces::Id ws) {
  switch (ws) {
    case workspaces::Id::Playlist:    return QStringLiteral("playlist");
    case workspaces::Id::PianoRoll:   return QStringLiteral("pianoroll");
    case workspaces::Id::ChannelRack: return QStringLiteral("channelrack");
    case workspaces::Id::Mixer:       return QStringLiteral("mixer");
    case workspaces::Id::Browser:     return QStringLiteral("browser");
    case workspaces::Id::Inspector:   return QStringLiteral("inspector");
    default: return QString();
  }
}
workspaces::Id wsIdFromString(const QString& id) {
  if (id == QStringLiteral("playlist"))    return workspaces::Id::Playlist;
  if (id == QStringLiteral("pianoroll"))   return workspaces::Id::PianoRoll;
  if (id == QStringLiteral("channelrack")) return workspaces::Id::ChannelRack;
  if (id == QStringLiteral("mixer"))       return workspaces::Id::Mixer;
  if (id == QStringLiteral("browser"))     return workspaces::Id::Browser;
  if (id == QStringLiteral("inspector"))   return workspaces::Id::Inspector;
  return workspaces::Id::Misc;
}

// Writes `json` (the session's serialized state) as the native .misuc ZIP
// container — misuc.meta (format/version) + misuc.song — atomically. This is
// the single write path for Save / Save As / autosave, so every document the
// app emits is a real container, never bare JSON text.
bool writeProjectDocument(const QString& path, const std::string& json,
                          std::string* err) {
  using misuc::core::ContainerEntry;
  using misuc::core::kContainerFormatVersion;
  using misuc::core::kContainerMetaEntry;
  using misuc::core::kContainerSongEntry;
  const std::string meta =
      std::string("{\"format\":\"misuc.container\",\"containerVersion\":") +
      std::to_string(kContainerFormatVersion) +
      ",\"app\":\"misuc\",\"modifiedUtc\":\"" + misuc::core::utcNowIso() + "\"}";
  std::vector<ContainerEntry> entries;
  entries.push_back({kContainerMetaEntry, meta});
  entries.push_back({kContainerSongEntry, json});
  return misuc::core::writeContainer(path.toStdString(), entries, err);
}

// Extracts the session JSON from any .misuc document: native ZIP container or
// a legacy plain-JSON file (older builds), which loads transparently and is
// migrated to the container format on the next save. Returns false with `err`
// set when the file is missing/corrupt or carries no session payload.
bool readProjectDocument(const QString& path, std::string& json,
                         std::string* err) {
  using misuc::core::ContainerEntry;
  using misuc::core::ContainerReadResult;
  using misuc::core::kContainerSongEntry;
  const ContainerReadResult r = misuc::core::readContainer(path.toStdString());
  if (!r.ok) {
    if (err) *err = r.error.empty() ? "cannot open project" : r.error;
    return false;
  }
  if (r.legacyJson) {
    json = r.legacyText;
    return true;
  }
  for (const ContainerEntry& e : r.entries) {
    if (e.name == kContainerSongEntry) {
      json = e.data;
      return true;
    }
  }
  if (err) *err = "project container has no session payload";
  return false;
}
} // namespace

MainWindow::MainWindow(misuc::engine::AudioEngine& engine) : engine_(engine) {
  resize(1280, 800);
  setMinimumSize(1000, 640);
  setAcceptDrops(true); // drop a .misuc project file onto the shell to open it
  updateWindowTitle(); // clean slate: "Untitled - Misuc" (no phantom dirty star)

  // paintEvent() fills the entire window each frame; skip the automatic
  // background-erase pass so live resizes repaint once, not twice.
  setAttribute(Qt::WA_OpaquePaintEvent);

  buildMenus();

  // Global header: brand, transport, live status cluster.
  headerBar_ = new HeaderBar(engine_, this);
  addToolBar(Qt::TopToolBarArea, headerBar_);
  headerBar_->setMovable(false);
  transport_ = headerBar_->transportBar();

  // Transport wiring (loop, record, metronome/count-in).
  connect(transport_, &TransportBar::loopToggled, this,
          &MainWindow::onLoopToggled);
  connect(transport_, &TransportBar::recordRequested, this,
          &MainWindow::onRecordRequested);
  // The click/count-in button is now a real metronome: it arms the engine's
  // metronome and count-in lead-in (previously dead UI). Default off so
  // playback/export stay click-free until the user enables it.
  connect(transport_, &TransportBar::countInToggled, this,
          [this](bool on) {
            session_.setMetronomeOn(on);
            engine_.setMetronome(on, session_.countInBeats(),
                                 session_.metronomeVolume());
          });
  connect(transport_, &TransportBar::metronomeVolumeChanged, this,
          [this](float v) {
            session_.setMetronomeVolume(v);
            engine_.setMetronome(session_.metronomeOn(), session_.countInBeats(),
                                 v);
          });
  connect(transport_, &TransportBar::countInBeatsChanged, this,
          [this](float b) {
            session_.setCountInBeats(b);
            engine_.setMetronome(session_.metronomeOn(), b,
                                 session_.metronomeVolume());
          });
  engine_.setMetronome(false, 2.f);

  // Central workspace: tabbed real pages.
  workspace_ = new QTabWidget(this);
  workspace_->setObjectName(QStringLiteral("workspaceTabs"));
  workspace_->setDocumentMode(true);
  workspace_->setTabPosition(QTabWidget::North);
  setCentralWidget(workspace_);
  wsMan_ = new WorkspaceManager(this, workspace_);
  wsMan_->setObjectName(QStringLiteral("wsMan"));
  buildWorkspaceViewMenu();

  // Status strip: contextual hint bar (left) + audio status (right). Created
  // before buildPages() so page wiring can register hints.
  statusBar()->setSizeGripEnabled(false);
  auto* hintBar = new HintBar(statusBar());
  hints_ = new HintManager(hintBar, this);
  statusBar()->addWidget(hintBar, 1);
  status_ = new StatusPanel(engine_, this);
  statusBar()->addPermanentWidget(status_);

  buildPages();

  // Appearance > Workspace layout: four real presets + Reset, kept in sync
  // with the live workspace state (presets check correctly, custom shows).
  themePage_->setupWorkspaceLayout(wsMan_->presetNames());
  themePage_->refreshWorkspacePreset(wsMan_->presetName());
  connect(themePage_, &ThemePage::workspacePresetSelected, this,
          &MainWindow::applyLayoutPreset);
  connect(themePage_, &ThemePage::workspaceResetRequested, this,
          [this] { wsMan_->resetWorkspace(); });
  connect(wsMan_, &WorkspaceManager::layoutChanged, this, [this] {
    syncLayoutActions();
    syncWorkspaceStrip();
    themePage_->refreshWorkspacePreset(wsMan_->presetName());
  });
  syncWorkspaceStrip();

  // Seed the undo history so the first edit has something to revert to.
  undo_.push(session_);
  rebuildMidiScore();
  rebuildClipScore();
  mixerPage_->syncToEngine();

  // P1: bring the audio backend up at launch (best effort) so Play/record work
  // immediately on a real device, instead of waiting for the Audio Settings
  // page. If the device is unavailable the engine stays down and the status
  // strip reports it honestly.
  audioSettings_->autoStart();

  // Workspace hotkeys (F5 Playlist, F6 Channel Rack, F7 Piano Roll,
  // F8 Browser, F9 Mixer) - each reveals and focuses its workspace.
  auto* playlistTab = new QAction(QStringLiteral("Playlist"), this);
  playlistTab->setShortcuts({QKeySequence(Qt::Key_F5)});
  connect(playlistTab, &QAction::triggered, this, [this] {
    wsMan_->showWorkspace(workspaces::Id::Playlist, true);
  });
  addAction(playlistTab);

  auto* rackTab = new QAction(QStringLiteral("Channel Rack"), this);
  rackTab->setShortcuts({QKeySequence(Qt::Key_F6)});
  connect(rackTab, &QAction::triggered, this, [this] {
    wsMan_->showWorkspace(workspaces::Id::ChannelRack, true);
  });
  addAction(rackTab);

  auto* pianoTab = new QAction(QStringLiteral("Piano Roll"), this);
  pianoTab->setShortcuts({QKeySequence(Qt::Key_F7)});
  connect(pianoTab, &QAction::triggered, this, [this] {
    wsMan_->showWorkspace(workspaces::Id::PianoRoll, true);
    editorPage_->piano()->setFocus(Qt::ShortcutFocusReason);
  });
  addAction(pianoTab);

  auto* browserTab = new QAction(QStringLiteral("Browser"), this);
  browserTab->setShortcuts({QKeySequence(Qt::Key_F8)});
  connect(browserTab, &QAction::triggered, this, [this] {
    wsMan_->showWorkspace(workspaces::Id::Browser, true);
  });
  addAction(browserTab);

  auto* mixerToggle = new QAction(QStringLiteral("mixerToggle"), this);
  mixerToggle->setShortcuts(
      {QKeySequence(Qt::Key_F9), QKeySequence(QStringLiteral("Ctrl+Shift+M"))});
  connect(mixerToggle, &QAction::triggered, this, [this] {
    wsMan_->showWorkspace(workspaces::Id::Mixer, !wsMan_->isVisible(workspaces::Id::Mixer));
  });
  addAction(mixerToggle);

  auto* inspectorToggle = new QAction(QStringLiteral("inspectorToggle"), this);
  inspectorToggle->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Shift+I"))});
  connect(inspectorToggle, &QAction::triggered, this, [this] {
    wsMan_->showWorkspace(workspaces::Id::Inspector,
                          !wsMan_->isVisible(workspaces::Id::Inspector));
  });
  addAction(inspectorToggle);

  auto* spaceAct = new QAction(QStringLiteral("playStop"), this);
  spaceAct->setShortcut(QKeySequence(Qt::Key_Space));
  connect(spaceAct, &QAction::triggered, this, &MainWindow::togglePlayStop);
  addAction(spaceAct);

  // Header strip <-> workspace manager: clicking a strip icon toggles the
  // workspace, and manager visibility changes keep the strip check states in
  // sync with the real dock/tab visibility.
  connect(headerBar_, &HeaderBar::workspaceIconClicked, this,
          [this](const QString& id) {
            auto ws = wsIdFromString(id);
            if (ws != workspaces::Id::Misc)
              wsMan_->toggleWorkspace(ws);
          });
  connect(headerBar_, &HeaderBar::projectRequested, this,
          [this] { wsMan_->showWorkspace(workspaces::Id::Project, true); });
  connect(wsMan_, &WorkspaceManager::workspaceToggled, this,
          [this](workspaces::Id ws, bool visible) {
            headerBar_->setWorkspaceActive(wsStripId(ws), visible);
          });
  connect(wsMan_, &WorkspaceManager::centralChanged, this,
          [this](workspaces::Id ws) {
            headerBar_->setWorkspaceActive(wsStripId(ws), true);
          });

  // Snap / song-pattern mode live on the header transport.
  connect(transport_, &TransportBar::snapToggled, this,
          [this](bool on) {
            headerBar_->setSnapActive(on);
            if (playlist_) playlist_->setSnap(on);
          });
  connect(transport_, &TransportBar::modeToggled, this,
          [this](bool song) {
            headerBar_->setSongMode(song);
            if (playlist_) playlist_->setSongMode(song);
          });

  buildCommands();
  restoreLayout();

  // Poll transport + status at 30 Hz.
  timer_ = new QTimer(this);
  timer_->setInterval(33);
  connect(timer_, &QTimer::timeout, this, [this] { poll(); });
  timer_->start();
  poll();

// Silent crash-recovery snapshot on the configured interval (default 90 s)
  // while the document is dirty. Not started until showEvent-less construction
  // completes; startup restore is offered from main() via
  // checkAutosaveRecovery(). The interval is a persisted app setting.
  autosaveTimer_ = new QTimer(this);
  connect(autosaveTimer_, &QTimer::timeout, this, &MainWindow::autosaveNow);
  applyAutosaveInterval(autosaveIntervalFromSettings());

  // Live per-lane automation record: sample armed lanes while the transport
  // runs at ~50 Hz (course enough for beat-snapped curves; points are
  // upserted against the exact transport position).
  automationRecTimer_ = new QTimer(this);
  automationRecTimer_->setInterval(20);
  connect(automationRecTimer_, &QTimer::timeout, this,
          &MainWindow::pollAutomationRecord);
  automationRecTimer_->start();
}

MainWindow::~MainWindow() {
  if (wsMan_)
    wsMan_->setTearingDown();
}

void MainWindow::buildMenus() {
  // File
  QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
  file->addAction(QStringLiteral("&New"), this, &MainWindow::newProject,
                  QKeySequence::New);
  file->addAction(QStringLiteral("&Open..."), this, &MainWindow::openDialog,
                  QKeySequence::Open);
  file->addAction(QStringLiteral("New &Demo Project..."), this,
                  &MainWindow::openDemoProject);
  file->addSeparator();
  recentMenu_ = file->addMenu(QStringLiteral("Open &Recent"));
  recentMenu_->setObjectName(QStringLiteral("recentMenu"));
  rebuildRecentMenu();
  file->addSeparator();
  file->addAction(QStringLiteral("&Save"), this, &MainWindow::saveProject,
                  QKeySequence::Save);
  file->addAction(QStringLiteral("Save &As..."), this,
                  &MainWindow::saveProjectAs, QKeySequence::SaveAs);
  file->addSeparator();
  file->addAction(QStringLiteral("Export &Audio..."), this,
                  &MainWindow::exportAudio, QKeySequence(Qt::CTRL | Qt::Key_E));
  file->addSeparator();
  file->addAction(QStringLiteral("&Quit"), this, &QWidget::close,
                  QKeySequence::Quit);

  // Edit / Add
  QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
  undoCmd_ = edit->addAction(QStringLiteral("&Undo"), this,
                             &MainWindow::performUndo,
                             QKeySequence::Undo);
  redoCmd_ = edit->addAction(QStringLiteral("&Redo"), this,
                             &MainWindow::performRedo,
                             QKeySequence::Redo);
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  edit->addSeparator();
  edit->addAction(QStringLiteral("&Snap to Grid"), this,
                  [this](bool on) {
                    if (transport_) transport_->setSnapOn(on);
                    if (playlist_) playlist_->setSnap(on);
                  },
                  QKeySequence(Qt::Key_N));
  edit->addSeparator();
  edit->addAction(QStringLiteral("Select &All"), this,
                  [this] { if (playlist_) playlist_->selectAll(); },
                  QKeySequence::SelectAll);
  edit->addAction(QStringLiteral("&Deselect"), this,
                  [this] { if (playlist_) playlist_->deselectAll(); },
                  QKeySequence(Qt::Key_Escape));
  edit->addAction(QStringLiteral("&Delete"), this,
                  [this] {
                    if (playlist_) {
                      auto items = playlist_->selectionCount();
                      if (items > 0) {
                        // Trigger delete via playlist's key handler
                        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
                        QApplication::sendEvent(playlist_, &del);
                      }
                    }
                  },
                  QKeySequence::Delete);

  // Add
  QMenu* addM = menuBar()->addMenu(QStringLiteral("&Add"));
  addM->addAction(QStringLiteral("&Import Audio..."), this,
                  [this] { if (playlist_) playlist_->importAudioDialog(); },
                  QKeySequence(Qt::CTRL | Qt::Key_I));
  addM->addAction(QStringLiteral("Channel &Rack Channel"), this,
                  [this] { if (channelRack_) channelRack_->addChannel(); });
  addM->addSeparator();
  addM->addAction(QStringLiteral("New &Track"), this,
                  [this] { editorPage_->addNewTrack(); });

  // Patterns (channel-rack-centric for now). Show-only conveniences; their
  // toggles (with F5-F9 shortcuts) live in View so no shortcut is duplicated.
  QMenu* patterns = menuBar()->addMenu(QStringLiteral("&Patterns"));
  patterns->addAction(QStringLiteral("Channel &Rack"), this,
                      [this] { wsMan_->showWorkspace(workspaces::Id::ChannelRack, true); });
  patterns->addAction(QStringLiteral("&Piano Roll"), this,
                      [this] { wsMan_->showWorkspace(workspaces::Id::PianoRoll, true); });

  // View: layout presets (four real arrangements) + workspace toggles
  QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
  QActionGroup* presetGroup = new QActionGroup(this);
  presetGroup->setExclusive(true);
  const QStringList presetList = wsMan_->presetNames();
  for (const QString& name : presetList) {
    QAction* act = view->addAction(name, this,
                                   [this, name] { applyLayoutPreset(name); });
    act->setCheckable(true);
    presetGroup->addAction(act);
    presetActions_ << act;
    if (name == QStringLiteral("Classic"))
      act->setChecked(true);
  }
  view->addAction(QStringLiteral("Reset Workspace"), this,
                  [this] { wsMan_->resetWorkspace(); });
  // Panel visibility toggles (F5-F9 etc.) are added by the workspace manager
  // once it exists; the View menu is remembered here for that step.
  viewMenu_ = view;

  // Options
  QMenu* options = menuBar()->addMenu(QStringLiteral("&Options"));
  metronomeCmd_ = options->addAction(QStringLiteral("&Metronome"), this,
      [this](bool on) { engine_.setMetronome(on, 2.f); transport_->setCountInOn(on); },
      QKeySequence(Qt::Key_M));
  metronomeCmd_->setCheckable(true);
  auto* loopOpt = options->addAction(QStringLiteral("&Loop Toggle"), this,
      [this](bool on) { onLoopToggled(on); }, QKeySequence(Qt::Key_L));
  loopOpt->setCheckable(true);
  auto* snapOpt = options->addAction(QStringLiteral("S&nap to Grid"), this,
      [this](bool on) {
        if (transport_) transport_->setSnapOn(on);
        if (playlist_) playlist_->setSnap(on);
      });
  snapOpt->setCheckable(true);
  snapOpt->setChecked(true);
  options->addSeparator();
  options->addAction(QStringLiteral("&Audio Settings"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Audio, true); });
  options->addAction(QStringLiteral("&Appearance"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Appearance, true); });

  // Tools
  QMenu* tools = menuBar()->addMenu(QStringLiteral("&Tools"));
  tools->addAction(QStringLiteral("&Project Setup"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Project, true); });
  tools->addAction(QStringLiteral("&Audio Console"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Audio, true); });
  tools->addSeparator();
  tools->addAction(QStringLiteral("Import &FLP..."), this,
      &MainWindow::importFlp);

  // Help
  QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
  help->addAction(QStringLiteral("&User Guide..."), this,
                  &MainWindow::showUserGuide);
  help->addAction(QStringLiteral("&About Misuc..."), this,
                  &MainWindow::showAbout);
}

void MainWindow::buildPages() {
  // Creation order matters: Project before Editor so the first QListWidget in
  // the window remains the editor's track list (embedded-library contract).
  projectPage_ = new ProjectPage(session_, this);
  editorPage_ = new EditorPage(session_, this);
  audioSettings_ = new AudioSettingsPage(engine_, this);
  // Session recovery settings (autosave interval + manual save-now arm drive
  // the MainWindow-owned autosave timer; see #7 Part 1).
  connect(audioSettings_, &AudioSettingsPage::autosaveIntervalChanged, this,
          [this](int seconds) { setAutosaveIntervalSeconds(seconds); });
  connect(audioSettings_, &AudioSettingsPage::autosaveNowRequested, this,
          [this] { autosaveNow(); });
  themePage_ = new ThemePage(this);
  mixerPage_ = new MixerPage(session_, engine_, this);
  playlist_ = new Playlist(session_, this);
  // FX control-port automation metadata (native range/default + label) for the
  // playlist's automation band: taken from the engine's effect catalog so the
  // "fx:<chain>:<port>" lanes can be scaled, labelled and enumerated.
  playlist_->setFxPortsResolver([this](const std::string& uri) {
    std::vector<FxPortInfo> out;
    const auto* f = engine_.fxInfo(uri);
    if (!f)
      return out;
    out.reserve(f->params.size());
    for (const auto& p : f->params) {
      FxPortInfo info;
      info.index = p.index;
      info.min = p.min;
      info.max = p.max;
      info.def = p.def;
      info.label = QString::fromStdString(p.label);
      out.push_back(std::move(info));
    }
    return out;
  });
  channelRack_ = new ChannelRack(session_, this);
  browser_ = new Browser(this);
  inspector_ = new Inspector(this);

  // Central tabs: Playlist first (the default home workspace), then the
  // document pages.
  wsMan_->addCentral(workspaces::Id::Playlist, playlist_);
  wsMan_->addCentral(workspaces::Id::PianoRoll, editorPage_);
  wsMan_->addCentral(workspaces::Id::Project, projectPage_);
  wsMan_->addCentral(workspaces::Id::Audio, audioSettings_);
  wsMan_->addCentral(workspaces::Id::Appearance, themePage_);

  // Dockable workspaces: Browser (left), Mixer (bottom), Inspector + Channel
  // Rack (right).
  wsMan_->addDock(workspaces::Id::Browser, browser_, Qt::LeftDockWidgetArea,
                  QStringLiteral("Browser"));
  wsMan_->addDock(workspaces::Id::Mixer, mixerPage_, Qt::BottomDockWidgetArea,
                  QStringLiteral("Mixer"));
  wsMan_->addDock(workspaces::Id::Inspector, inspector_,
                  Qt::RightDockWidgetArea, QStringLiteral("Inspector"));
  wsMan_->addDock(workspaces::Id::ChannelRack, channelRack_,
                  Qt::RightDockWidgetArea, QStringLiteral("Channel Rack"));
  mixerDock_ = wsMan_->docks(workspaces::Id::Mixer);
  if (mixerDock_)
    mixerDock_->setObjectName(QStringLiteral("mixerDock"));

  editorPage_->importAudioSink = [this](const QString& src) {
    return stageAudioFile(src);
  };

  connect(projectPage_, &ProjectPage::edited, this,
          &MainWindow::onSessionEdited);
  connect(editorPage_, &EditorPage::edited, this,
          &MainWindow::onSessionEdited);
  connect(mixerPage_, &MixerPage::edited, this,
          &MainWindow::onSessionEdited);
  connect(playlist_, &Playlist::edited, this, &MainWindow::onSessionEdited);
  connect(playlist_, &Playlist::bandDragFinished, this,
          &MainWindow::onBandDragFinished);
  connect(inspector_, &Inspector::editRequest, this,
          &MainWindow::onInspectorEdit);
  connect(channelRack_, &ChannelRack::edited, this, &MainWindow::onSessionEdited);
  connect(channelRack_, &ChannelRack::edited, this, [this] {
    // With pattern playback armed, republish the freshly-synced steps so
    // edits audition live through the real audio path.
    if (rackPatternLive_)
      rebuildRackScore();
  });
  connect(channelRack_, &ChannelRack::patternLiveChanged, this,
          &MainWindow::onRackPatternLive);

  // Track selection is mirrored between the editor list, the mixer strips,
  // the playlist lanes and the inspector; echo states are never re-broadcast.
  auto selectTrackEverywhere = [this](QString id, bool notifyEditor) {
    mixerPage_->selectTrack(id);
    playlist_->selectTrack(id);
    editorPage_->selectTrack(id, notifyEditor);
    headerBar_->setWorkspaceActive(QStringLiteral("pianoroll"), true);
  };
  connect(editorPage_, &EditorPage::trackSelected, this,
          [this, selectTrackEverywhere](const QString& id) {
            selectTrackEverywhere(id, false);
          });
  connect(mixerPage_, &MixerPage::trackSelected, this,
          [this, selectTrackEverywhere](const QString& id) {
            selectTrackEverywhere(id, false);
          });
  connect(playlist_, &Playlist::trackSelected, this,
          [this, selectTrackEverywhere](const QString& id) {
            selectTrackEverywhere(id, true);
          });

  // Note audition: preview the pitch immediately, even while stopped.
  connect(editorPage_->piano(), &PianoRoll::audition, this,
          [this](double freq, float vel) { engine_.previewNote(freq, vel); });

  // Ruler click in the piano roll parks the transport caret (seek) so you can
  // drop the playhead before pressing Play — a real, engine-driven seek.
  connect(editorPage_->piano(), &PianoRoll::seekRequested, this,
          [this](double beat) { engine_.seekToBeat(beat); });

  // Playlist ruler click also seeks the engine transport caret.
  connect(playlist_, &Playlist::seekRequested, this,
          [this](double beat) { engine_.seekToBeat(beat); });

  // Starter sounds: audition the selected channel's base pitch through the real
  // engine voice at its channel volume (no fabricated samples — honest preview).
  connect(channelRack_, &ChannelRack::previewRequested, this,
          [this](int row) {
            if (row < 0)
              return;
            const auto& ch = channelRack_->channels();
            if (row >= ch.size())
              return;
            // A sampler lane with a sound asset auditions the dropped sample
            // (honest file playback); the rest use the engine voice pitch.
            const QString sf = ch[row].soundFile;
            if (!sf.isEmpty() && QFileInfo(sf).exists()) {
              engine_.previewStop();
              auto hit = rackSampleCache_.constFind(sf);
              if (hit == rackSampleCache_.constEnd()) {
                misuc::audio::AudioBuffer buf;
                std::string err;
                if (misuc::audio::readAudioFile(sf.toStdString(), buf, err) &&
                    !buf.samples.empty()) {
                  rackSampleCache_.insert(sf, std::move(buf));
                  hit = rackSampleCache_.constFind(sf);
                }
              }
              if (hit != rackSampleCache_.constEnd()) {
                const float g =
                    std::clamp(ch[row].volume, 0.05f, 1.0f);
                engine_.previewSample(hit->samples, hit->channels, g);
              }
              return;
            }
            const double freq = channelRack_->previewFreq(row);
            float vel = 0.7f;
            if (row < ch.size() && ch[row].volume > 0.0f)
              vel = std::clamp(ch[row].volume, 0.05f, 1.0f);
            engine_.previewNote(freq, vel);
          });

  // Place Pattern: materialize the active step bank into a real session MIDI
  // region (4 beats, one 1/16 gate per step) on the "Sequencer" track, so it
  // becomes fully editable in the piano roll and arrangement alike.
  connect(channelRack_, &ChannelRack::placePatternRequested, this, [this] {
    std::string trackId;
    for (auto& t : session_.tracks())
      if (t.name == "Sequencer")
        trackId = t.id;
    if (trackId.empty())
      trackId = session_.addTrack("Sequencer");
    auto* t = session_.findTrack(trackId);
    if (!t)
      return;
    double start = 0.0;
    for (auto& r : t->regions)
      start = std::max(start, r.startBeats + r.durationBeats);
    for (auto& c : t->clips)
      start = std::max(start, c.startBeats + c.durationBeats);
    const int bank = channelRack_->pattern();
    misuc::project::materializePatternRegion(
        session_, trackId, bank, start, 4.0);
    t = session_.findTrack(trackId);
    if (playlist_) {
      playlist_->selectTrack(QString::fromStdString(trackId));
      playlist_->refresh();
    }
    onSessionEdited();
    wsMan_->showWorkspace(workspaces::Id::Playlist, true);
  });
  // Real sample audition: the selected browser file plays through the engine's
// one-shot sample preview pipeline (honest playback, not a synthesized beep).
  connect(browser_, &Browser::previewFile, this, [this](const QString& path) {
    misuc::audio::AudioBuffer b;
    std::string err;
    if (!misuc::audio::readAudioFile(path.toStdString(), b, err))
      return;
    // A new audition always kills the previous one first: two previews must
    // never sound at once (master spec §4: clicking another sound replaces
    // the previous preview).
    engine_.previewStop();
    const float vol =
        static_cast<float>(qBound(0.0, browser_->previewVolume(), 2.0));
    engine_.previewSample(std::move(b.samples),
                          std::max<uint32_t>(1, b.channels), vol);
  });
  // Preview strip transport/volume → the engine's preview controls.
  connect(browser_, &Browser::previewPauseChanged, this,
          [this](bool paused) { engine_.previewPause(paused); });
  connect(browser_, &Browser::previewStopRequested, this,
          [this] { engine_.previewStop(); });
  connect(browser_, &Browser::previewVolumeChanged, this,
          [this](double v) {
            engine_.previewGain(static_cast<float>(qBound(0.0, v, 2.0)));
          });

  // Playlist → piano-roll handoff: Open in Piano Roll (region-aware).
  connect(playlist_, &Playlist::openInPianoRoll, this,
          [this](const QString& trackId, const QString& regionId) {
            wsMan_->showWorkspace(workspaces::Id::PianoRoll, true);
            editorPage_->selectTrack(trackId, true);
            editorPage_->piano()->setRegion(trackId, regionId);
            editorPage_->piano()->setFocus(Qt::ShortcutFocusReason);
          });

  // Inspector is driven live by the current selection.
  connect(playlist_, &Playlist::selectionChanged, this,
          &MainWindow::refreshInspector);

  // Browser: dropping/inserting a sample into the current track.
  connect(browser_, &Browser::insertSample, this, [this](const QString& src) {
    const double bpm = session_.tempo();
    double beat = 0.0;
    if (bpm > 0.0 && engine_.sampleRate() > 0)
      beat = static_cast<double>(engine_.playheadSamples()) /
             ((60.0 / bpm) * engine_.sampleRate());
    int row = playlist_->selectedClipRow();
    playlist_->insertSampleAt(src, beat, qMax(0, row));
  });

  // Wire contextual hints over the chrome and workspace controls.
  hints_->registerHint(projectPage_, QStringLiteral("Project"),
                       QStringLiteral("Project name, tempo and time signature"),
                       QStringLiteral("Ctrl+N/O/S"));
  hints_->registerHint(editorPage_, QStringLiteral("Piano Roll"),
                       QStringLiteral("Draw and edit MIDI notes"),
                       QStringLiteral("F7"));
  hints_->registerHint(mixerPage_, QStringLiteral("Mixer"),
                       QStringLiteral("Per-track gain/pan/mute/solo and meters"),
                       QStringLiteral("F9"));
  hints_->registerHint(playlist_, QStringLiteral("Playlist"),
                       QStringLiteral(
                           "Arrange audio clips and MIDI regions; move, resize, "
                           "trim, split, mute, duplicate, import, drag from the "
                           "browser"),
                       QStringLiteral("F5"));
  hints_->registerHint(channelRack_, QStringLiteral("Channel Rack"),
                       QStringLiteral(
                           "Step-sequencer channels with gates, volume and pan; "
                           "click a step to gate it"),
                       QStringLiteral("F6"));
  hints_->registerHint(browser_, QStringLiteral("Browser"),
                       QStringLiteral("Samples, project audio, instruments, "
                                      "effects, MIDI and plugs"),
                       QStringLiteral("F8"));
hints_->registerHint(inspector_, QStringLiteral("Inspector"),
                        QStringLiteral("Context properties of the selection"),
                        QStringLiteral("F4"));
  hints_->registerHint(audioSettings_, QStringLiteral("Audio Settings"),
                        QStringLiteral("Device, sample rate, buffer and monitoring"),
                        QString(), nullptr);
  hints_->registerHint(themePage_, QStringLiteral("Appearance"),
                        QStringLiteral("Theme presets, accent and glass"),
                        QString(), nullptr);

  // Icon-only chrome registers its own tool hints per widget so the HintBar
  // (§78) behaves like a full app-level status line with live values.
  playlist_->attachHints(hints_);
  channelRack_->attachHints(hints_);
  browser_->attachHints(hints_);
  transport_->attachHints(hints_);
  headerBar_->attachHints(hints_);
  mixerPage_->attachHints(hints_);
  inspector_->attachHints(hints_);
  editorPage_->attachHints(hints_);
  projectPage_->attachHints(hints_);
  themePage_->attachHints(hints_);
  audioSettings_->attachHints(hints_);

  applyContentEndToEngine();
  mixerPage_->syncToEngine();
}

void MainWindow::onSessionEdited() {
  undo_.push(session_);
  dirty_ = true;
  applyTempoToEngine();
  applyLoopToEngine(session_);
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  applyAutomationToEngine();
  applyFxToEngine();
  // Input monitoring follows arming: the live input is mixed to the output
  // whenever at least one track is record-armed (capture-only otherwise).
  engine_.setInputMonitor(anyRecordArmed());
  mixerPage_->syncFromSession();
  mixerPage_->syncToEngine();
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  updateWindowTitle();
  if (playlist_) playlist_->refresh();
  if (browser_) {
    QStringList srcs;
    for (auto& t : session_.tracks())
      for (auto& c : t.clips)
        if (!c.sourceFile.empty())
          srcs << QString::fromStdString(c.sourceFile);
    browser_->setSessionSources(srcs);
  }
  refreshInspector();
}

void MainWindow::rebuildMidiScore() {
  std::vector<misuc::dsp::MidiNote> notes;
  misuc::engine::buildMidiScore(session_, notes);
  engine_.setScore(std::move(notes));
}

void MainWindow::rebuildClipScore() {
  std::vector<misuc::dsp::PlaybackClip> clips;
  std::vector<float> storage;
  std::string err;
  misuc::engine::buildClipScore(session_, clips, storage, err);
  engine_.setClips(std::move(clips), std::move(storage));
}

void MainWindow::applyAutomationToEngine() {
  std::vector<misuc::engine::AutomationParam> params;
  uint32_t trackIdx = 0;
  for (const auto& t : session_.tracks()) {
    for (const auto& lane : t.automation) {
      // All automatable params publish (recently including fx control-port
      // lanes "fx:<chain>:<port>"); the engine parses each and scales them
      // natively, skipping any lane whose chain/port is out of range.
      misuc::engine::AutomationParam a;
      a.trackIndex = trackIdx;
      a.param = lane.param;
      a.active = !lane.muted && !lane.points.empty();
      for (const auto& p : lane.points) {
        a.times.push_back(p.timeBeats);
        a.values.push_back(static_cast<float>(p.value));
      }
      params.push_back(std::move(a));
    }
    ++trackIdx;
  }
  engine_.publishAutomation(std::move(params));
}

// Live value of an fx control port: the insert's current parameter value if
// that port exists on the targeted insert, otherwise `fallback` (keeps an
// armed fx lane flat until the rack control actually moves).
static double currentFxParamValue(const misuc::project::Track& t,
                                  uint32_t chain, uint32_t port,
                                  double fallback) {
  if (chain < static_cast<uint32_t>(t.fx.size())) {
    for (const auto& p : t.fx[chain].params)
      if (p.index == port)
        return static_cast<double>(p.value);
  }
  return fallback;
}

void MainWindow::pollAutomationRecord() {
  if (!engine_.transport().isPlaying()) {
    autoRecUndoLatch_ = false;
    bandTape_.clear();
    return;
  }
  const double fpb = engine_.transport().framesPerBeat();
  if (fpb <= 0.0)
    return;
  const double beat = engine_.transport().positionFrames() / fpb;
  bool wrote = false;
  int dragRow = -1, dragAutoIdx = -1;
  double dragVal = 0.0;
  const bool dragging = playlist_ && playlist_->liveBandDrag(dragRow, dragAutoIdx,
                                                            dragVal);
  for (size_t ti = 0; ti < session_.tracks().size(); ++ti) {
    auto& t = session_.tracks()[ti];
    // Live control source for each automatable param: the track's fader
    // (linear amplitude) and pan position from the mixer model.
    const double gainAmp =
        std::clamp(std::pow(10.0, t.gainDb / 20.0), 0.0, 2.0);
    for (size_t ai = 0; ai < t.automation.size(); ++ai) {
      auto& lane = t.automation[ai];
      if (!lane.armed || lane.muted)
        continue;
      // Phase Q: the band is currently being dragged on this exact lane. The
      // dragged point's live value is tapped at the transport beat into the
      // pending tape (merged once at drag release); the lane itself is left
      // untouched here so the in-progress drag never races an upsampling write.
      if (dragging && dragRow == static_cast<int>(ti) &&
          dragAutoIdx == static_cast<int>(ai)) {
        for (auto it = bandTape_.begin(); it != bandTape_.end();) {
          if (std::abs(it->timeBeats - beat) < 0.01)
            it = bandTape_.erase(it);
          else
            ++it;
        }
        bandTape_.push_back({beat, dragVal});
        continue;
      }
      double value;
      if (misuc::engine::parseFxAutomation(lane.param).ok) {
        // FX control-port automation: the live source is the insert's current
        // parameter value, surfaced by the Inspector when the user is "driving"
        // the rack while Play+Record capture runs. Falls back to the lane's
        // last written value so a recording stays flat until the rack moves.
        const auto fa = misuc::engine::parseFxAutomation(lane.param);
        const double fallback =
            lane.points.empty() ? 0.0 : lane.points.back().value;
        value = currentFxParamValue(t, fa.chainIndex, fa.portIndex, fallback);
      } else {
        value = (lane.param == "pan")
                    ? static_cast<double>(t.pan)
                    : (lane.param == "volume")
                          ? std::clamp(gainAmp, 0.0, 1.0)
                          : gainAmp;
      }
      // Upsert a point at the exact transport beat: drop any point within a
      // hair of this beat (so loop wraps do not pile up values), then insert
      // sorted.
      for (auto it = lane.points.begin(); it != lane.points.end();) {
        if (std::abs(it->timeBeats - beat) < 0.01)
          it = lane.points.erase(it);
        else
          ++it;
      }
      misuc::project::AutomationPoint np;
      np.timeBeats = beat;
      np.value = value;
      auto pos = std::lower_bound(
          lane.points.begin(), lane.points.end(), np,
          [](const misuc::project::AutomationPoint& a,
             const misuc::project::AutomationPoint& b) {
            return a.timeBeats < b.timeBeats;
          });
      lane.points.insert(pos, np);
      if (!autoRecUndoLatch_) {
        // Snapshot once per pass so the whole recording undoes as one step.
        undo_.push(session_);
        autoRecUndoLatch_ = true;
      }
      wrote = true;
    }
  }
  if (wrote) {
    applyAutomationToEngine();
    if (playlist_)
      playlist_->refresh();
  }
}

// Phase Q: fold the captured band-drag tape into the dragged lane. Runs just
// before the grid's edited(), so the drag and all its taped points land in the
// SAME undo step (the merge mutates the session, then edited() snapshots).
void MainWindow::onBandDragFinished(int row, int autoIdx) {
  if (bandTape_.empty()) {
    bandTape_.clear();
    return;
  }
  if (row >= 0 && row < static_cast<int>(session_.tracks().size())) {
    auto& t = session_.tracks()[static_cast<size_t>(row)];
    if (autoIdx >= 0 && autoIdx < static_cast<int>(t.automation.size())) {
      auto& lane = t.automation[static_cast<size_t>(autoIdx)];
      for (const misuc::project::AutomationPoint& tp : bandTape_) {
        for (auto it = lane.points.begin(); it != lane.points.end();) {
          if (std::abs(it->timeBeats - tp.timeBeats) < 0.01)
            it = lane.points.erase(it);
          else
            ++it;
        }
        auto pos = std::lower_bound(
            lane.points.begin(), lane.points.end(), tp,
            [](const misuc::project::AutomationPoint& a,
               const misuc::project::AutomationPoint& b) {
              return a.timeBeats < b.timeBeats;
            });
        lane.points.insert(pos, tp);
      }
    }
  }
  bandTape_.clear();
}

void MainWindow::onRackPatternLive(bool live) {
  rackPatternLive_ = live;
  if (live) {
    // Pattern playback owns the engine score and loops its own 4 beats (one
    // 16-step lane) through the real-time transport. Start the transport so
    // the loop actually spins.
    if (!engine_.isTransportPlaying()) {
      // Ensure the engine knows a playable span exists before Play is drained:
      // the transport's corpus runs 0..4 under the loop, so content end = 4.
      engine_.setContentEnd(4.0);
      engine_.play(false);
    }
    rebuildRackScore();
    engine_.setLoop(true, 0.0, 4.0);
  } else {
    // Hand the score back to the session document and restore its loop.
    rebuildMidiScore();
    // The rack's sample clips owned the engine clip score while pattern-live;
    // hand it back to the session's clips so normal playback has its audio.
    rebuildClipScore();
    applyLoopToEngine(session_);
    // Republish the session's real mixer state so the rack's live-mode
    // mixer params (pan/mute/solo per row) don't persist into normal playback.
    mixerPage_->syncToEngine();
  }
}

void MainWindow::rebuildRackScore() {
  std::vector<misuc::dsp::MidiNote> notes;
  std::vector<misuc::dsp::PlaybackClip> clips;
  std::vector<float> sampleStorage;
  std::vector<size_t> clipStorageOffset;
  std::map<std::string, bool> pathDecoded; // one decode copy per unique file
  const auto& ch = channelRack_->channels();
  const int bank = channelRack_->pattern();
  const double bpm = session_.tempo() > 0.0 ? session_.tempo() : 120.0;
  // Swing delays the off-beat 16ths toward the following step (per cent of a
  // sixteenth); main beats stay put, odd steps shift.
  const double swingShift = 0.25 * (channelRack_->swing() / 100.0);
  bool anySolo = false;
  for (const auto& c : ch)
    if (c.solo) anySolo = true;
  for (int row = 0; row < ch.size(); ++row) {
    const ChannelRack::Channel& c = ch[row];
    if (c.muted)
      continue;
    if (anySolo && !c.solo)
      continue;
    bool anyActive = false;
    for (int step = 0; step < 16; ++step)
      if (step < c.steps[bank].size() && c.steps[bank][step]) {
        anyActive = true;
        break;
      }
    if (!anyActive)
      continue;
    // A channel carrying a sound asset is a real sampler lane: its gated steps
    // audition the actual sample through the engine's clip player (honest
    // playback of the dropped file, not a synthesized pitch). Channels without
    // an asset keep the built-in instrument voice via the MIDI score.
    const std::string sf = c.soundFile.toStdString();
    bool haveSample = false;
    size_t off = 0;
    uint32_t sFrames = 0, sChannels = 1, sRate = 48000;
    double sDurBeats = 1.0;
    if (!sf.empty() && QFileInfo(c.soundFile).exists()) {
      const auto it = pathDecoded.find(sf);
      if (it == pathDecoded.end()) {
        bool ok = false;
        auto hit = rackSampleCache_.constFind(c.soundFile);
        if (hit == rackSampleCache_.constEnd()) {
          misuc::audio::AudioBuffer buf;
          std::string err;
          if (misuc::audio::readAudioFile(sf, buf, err) && buf.frames > 0 &&
              buf.sampleRate > 0.0 &&
              buf.samples.size() ==
                  static_cast<size_t>(buf.frames) *
                      static_cast<size_t>(buf.channels)) {
            rackSampleCache_.insert(c.soundFile, std::move(buf));
            hit = rackSampleCache_.constFind(c.soundFile);
            ok = true;
          }
        } else {
          ok = true;
        }
        if (ok) {
          off = sampleStorage.size();
          sampleStorage.insert(sampleStorage.end(), hit->samples.begin(),
                               hit->samples.end());
          sFrames = static_cast<uint32_t>(hit->frames);
          sChannels =
              static_cast<uint32_t>(std::max<uint16_t>(1, hit->channels));
          sRate = static_cast<uint32_t>(hit->sampleRate);
          sDurBeats = hit->durationSeconds() * bpm / 60.0;
          haveSample = true;
        }
        pathDecoded.emplace(sf, ok);
      } else if (it->second) {
        const auto hit = rackSampleCache_.constFind(c.soundFile);
        if (hit != rackSampleCache_.constEnd()) {
          off = sampleStorage.size();
          sampleStorage.insert(sampleStorage.end(), hit->samples.begin(),
                               hit->samples.end());
          sFrames = static_cast<uint32_t>(hit->frames);
          sChannels =
              static_cast<uint32_t>(std::max<uint16_t>(1, hit->channels));
          sRate = static_cast<uint32_t>(hit->sampleRate);
          sDurBeats = hit->durationSeconds() * bpm / 60.0;
          haveSample = true;
        }
      }
    }
    for (int step = 0; step < 16; ++step) {
      if (step >= c.steps[bank].size() || !c.steps[bank][step])
        continue;
      double start = step * 0.25;
      if ((step & 1) && swingShift > 0.0)
        start += swingShift;
      if (haveSample) {
        misuc::dsp::PlaybackClip pc;
        pc.channels = sChannels;
        pc.frames = sFrames;
        pc.sampleRate = sRate;
        pc.startBeats = start;
        pc.durationBeats = sDurBeats;
        pc.track = static_cast<uint32_t>(row);
        pc.gain = std::clamp(
            c.volume * (static_cast<float>(c.steps[bank][step]) / 127.f),
            0.05f, 2.0f);
        pc.samples = nullptr; // repointed once storage is final
        clips.push_back(pc);
        clipStorageOffset.push_back(off);
      } else {
        misuc::dsp::MidiNote mn;
        mn.startBeats = start;
        mn.durationBeats = 0.20;
        mn.freq = channelRack_->previewFreq(row);
        // Channel volume × this step's velocity (0..127). The engine mixer
        // param published alongside stays at unity gain; per-note gain carries
        // both factors here.
        mn.gain = c.volume *
                  (static_cast<float>(c.steps[bank][step]) / 127.f);
        mn.track = static_cast<uint32_t>(row);
        notes.push_back(mn);
      }
    }
  }
  std::sort(notes.begin(), notes.end(),
            [](const misuc::dsp::MidiNote& a, const misuc::dsp::MidiNote& b) {
              return a.startBeats < b.startBeats;
            });
  // Repoint every clip's sample pointer into the (now final) storage buffer.
  if (!sampleStorage.empty() && !clips.empty()) {
    const float* base = sampleStorage.data();
    for (size_t i = 0; i < clips.size(); ++i)
      clips[i].samples = base + clipStorageOffset[i];
  }
  engine_.setScore(std::move(notes));
  engine_.setClips(std::move(clips), std::move(sampleStorage));
  // Map each rack row's volume/pan onto its playback track in the engine's
  // mixer so the pattern lane audibly honors per-channel controls. Only while
  // pattern-live playback owns the score; when it stops, the session's own
  // mixer state (republished by the mixer/transport) takes over again.
  if (rackPatternLive_) {
    std::vector<misuc::engine::MixerTrackParam> params(ch.size());
    for (int row = 0; row < ch.size(); ++row) {
      const ChannelRack::Channel& c = ch[row];
      params[row].gainDb = 1.0f;   // per-note gain already carries volume
      params[row].pan = c.pan;
      params[row].mute = c.muted;
      params[row].solo = anySolo && !c.solo;
    }
    engine_.publishMixer(std::move(params));
  }
}

void MainWindow::onRecordRequested() {
  // Show Record Setup dialog
  auto inputDevices = engine_.backend()->enumerateDevices(true);
  std::vector<std::string> trackNames;
  for (const auto& t : session_.tracks()) {
    trackNames.push_back(t.name);
  }
  int currentTrackIndex = -1;
  if (editorPage_) {
    auto trackId = editorPage_->piano()->trackId().toStdString();
    if (!trackId.empty()) {
      auto* t = session_.findTrack(trackId);
      if (t) {
        for (size_t i = 0; i < session_.tracks().size(); ++i) {
          if (session_.tracks()[i].id == trackId) {
            currentTrackIndex = (int)i;
            break;
          }
        }
      }
    }
  }

  RecordSetupDialog dialog(inputDevices, projectPath_, trackNames, currentTrackIndex, this);
  if (dialog.exec() != QDialog::Accepted) {
    return; // User cancelled
  }

  auto config = dialog.getConfig();

  // Update engine config with selected input device
  misuc::engine::AudioConfig engineConfig = engine_.backend()->config();
  engineConfig.inputDeviceId = config.inputDeviceId;
  engineConfig.inputChannels = config.inputChannels;
  engineConfig.sampleRate = config.sampleRate;

  std::string error;
  if (!engine_.backend()->open(engineConfig, error)) {
    QMessageBox::warning(this, "Record Setup Failed", QString::fromStdString(error));
    return;
  }

  // Start recording
  transport_->setRecordingState(true);
  engine_.setInputMonitor(config.inputMonitor);

  // Count-in handling
  if (config.countInBars > 0 && transport_) {
    transport_->setCountInOn(true);
    transport_->setCountInBeats(config.countInBars * 4.0f); // bars to beats
  }
  session_.setMetronomeOn(config.metronomeOn);
  if (config.metronomeOn) {
    engine_.setMetronome(true, session_.countInBeats(), session_.metronomeVolume());
  }

  if (!engine_.isTransportPlaying()) {
    engine_.play(config.countInBars > 0); // play with count-in if enabled
  }
  engine_.startRecord();

  // Store recording config for when we stop
  MainWindow::RecordConfig mwConfig;
  mwConfig.type = MainWindow::RecordConfig::Type::Audio;
  mwConfig.inputDeviceId = config.inputDeviceId;
  mwConfig.inputDeviceName = config.inputDeviceName;
  mwConfig.inputChannels = config.inputChannels;
  mwConfig.sampleRate = config.sampleRate;
  mwConfig.format = config.format;
  mwConfig.recordPath = config.recordPath;
  mwConfig.fileName = config.fileName;
  mwConfig.countInBars = config.countInBars;
  mwConfig.metronomeOn = config.metronomeOn;
  mwConfig.destinationTrack = config.destinationTrack;
  mwConfig.createNewTrack = config.createNewTrack;
  mwConfig.inputMonitor = config.inputMonitor;
  currentRecordConfig_ = mwConfig;
}

void MainWindow::onRecordStopped() {
  transport_->setRecordingState(false);
  engine_.stopRecord();
  engine_.setInputMonitor(anyRecordArmed());

  misuc::engine::RecordCapture cap;
  if (!engine_.takeRecord(cap) || cap.sampleRate == 0 || cap.channels == 0 ||
      cap.samples.empty())
    return;

  if (!currentRecordConfig_.has_value()) {
    return;
  }

  auto config = currentRecordConfig_.value();
  currentRecordConfig_.reset();

  // Write recording to file
  ++recordCounter_;
  QString baseName = QString::fromStdString(config.fileName);
  QString wavName = baseName + QStringLiteral(".%1").arg(config.format == "flac" ? "flac" : "wav");
  QString projectHome =
      projectPath_.isEmpty()
          ? QDir::tempPath()
          : QFileInfo(projectPath_).absolutePath();
  QDir().mkpath(QDir(projectHome).filePath(QStringLiteral("audio")));
  QString audioDir = QDir(projectHome).filePath("audio");
  QString wavPath = QDir(audioDir).filePath(wavName);

  // Ensure unique filename
  int counter = 1;
  QString originalPath = wavPath;
  while (QFile::exists(wavPath)) {
    wavPath = originalPath;
    wavPath.insert(wavPath.lastIndexOf('.'), QString("_%1").arg(counter++));
  }

  std::string werr;
  bool writeOk = false;
  if (config.format == "flac") {
    // For FLAC, write WAV first then convert, or use FLAC directly
    // For now, write as WAV
    writeOk = misuc::audio::writeWavSamples(wavPath.toStdString(), cap.samples.data(),
                                            cap.samples.size(), config.inputChannels,
                                            cap.sampleRate, false, werr);
  } else {
    writeOk = misuc::audio::writeWavSamples(wavPath.toStdString(), cap.samples.data(),
                                            cap.samples.size(), config.inputChannels,
                                            cap.sampleRate, false, werr);
  }
  if (!writeOk) {
    QMessageBox::warning(this, "Recording Failed", QString::fromStdString(werr));
    return;
  }

  // Determine target track
  std::string tid;
  if (config.destinationTrack >= 0 && config.destinationTrack < (int)session_.tracks().size()) {
    tid = session_.tracks()[config.destinationTrack].id;
  } else if (config.createNewTrack) {
    tid = session_.addTrack("Audio " + std::to_string(session_.tracks().size() + 1));
  } else {
    tid = recordTargetTrack();
    if (tid.empty()) {
      if (session_.tracks().empty())
        session_.addTrack("Track 1");
      tid = session_.tracks().front().id;
    }
  }

  // Create AudioClip
  misuc::project::AudioClip clip;
  clip.name = config.fileName;
  clip.sourceFile = wavPath.toStdString();
  clip.startBeats = cap.startBeats;
  clip.durationBeats =
      cap.sampleRate > 0
          ? std::max(0.01, (cap.samples.size() / static_cast<double>(cap.sampleRate)) *
                                session_.tempo() / 60.0)
          : 0.5;
  clip.sampleRate = cap.sampleRate;
  clip.channels = config.inputChannels;
  const std::string clipId = session_.addClip(tid, std::move(clip));

  // Register the take
  misuc::project::RecordTake take;
  take.name = "Take " + std::to_string(session_.takes(tid).size() + 1);
  take.sourceFile = wavPath.toStdString();
  take.startBeats = cap.startBeats;
  take.durationBeats = clip.durationBeats;
  take.sampleRate = cap.sampleRate;
  take.channels = config.inputChannels;
  take.clipId = clipId;
  session_.addTake(tid, take);

  onSessionEdited();
  transport_->setRecordingState(false);
}

// First record-armed track wins for placement; otherwise the planted piano
// track; otherwise nothing (a fresh track is created by the caller).
std::string MainWindow::recordTargetTrack() const {
  for (const misuc::project::Track& t : session_.tracks())
    if (t.recordArm)
      return t.id;
  const std::string selected =
      editorPage_->piano()->trackId().toStdString();
  if (session_.findTrack(selected))
    return selected;
  return std::string();
}

bool MainWindow::anyRecordArmed() const {
  for (const misuc::project::Track& t : session_.tracks())
    if (t.recordArm)
      return true;
  return false;
}

void MainWindow::placeTake(misuc::project::Track& tr,
                           misuc::project::RecordTake& take) {
  // Drop every clip on this track that materialized a session take (each in
  // turn clears the owning take's reference), so the timeline ends with
  // exactly one take's audio — switching takes, comp style.
  for (auto it = tr.clips.begin(); it != tr.clips.end();) {
    bool isTakeClip = false;
    for (auto& k : tr.takes) {
      if (k.clipId == it->id) {
        k.clipId.clear();
        isTakeClip = true;
        if (&k == &take)
          take.clipId.clear();
      }
    }
    if (isTakeClip)
      it = tr.clips.erase(it);
    else
      ++it;
  }
  misuc::project::AudioClip c;
  c.name = take.name;
  c.sourceFile = take.sourceFile;
  c.startBeats = take.startBeats;
  c.durationBeats = take.durationBeats;
  c.gainDb = take.gainDb;
  c.sampleRate = take.sampleRate;
  c.channels = take.channels;
  take.clipId = session_.addClip(tr.id, c);
}

void MainWindow::exportAudio() {
  // Headless override: straight to the configured bounce, no dialog.
  if (!exportPathOverride_.isEmpty()) {
    int format = static_cast<int>(misuc::audio::ExportFormat::WavFloat);
    if (exportOverrideOptions_)
      format = exportOverrideFormat_;
    const int rangeKind =
        exportOverrideOptions_ && exportOverrideLoop_ ? 1 : 0;
    const bool dither = exportOverrideOptions_ && exportOverrideDither_;
    doExport(exportPathOverride_,
             static_cast<misuc::audio::ExportFormat>(format), rangeKind,
             dither, QString::fromStdString(session_.name()),
             QString::fromStdString(session_.artist()),
             exportOverrideSampleRate_ != 0
                 ? exportOverrideSampleRate_
                 : session_.sampleRate(),
             exportOverrideMetronome_);
    return;
  }

  // Interactive: pick format / range / dither / metadata / rate, then bounce.
  bool loopAvailable = session_.loopMode() &&
                       session_.loopEnd() > session_.loopStart();
  const auto sel = playlist_ ? playlist_->selectionSummary()
                             : Playlist::SelectionSummary{};
  const bool selectionAvailable =
      sel.durationBeats > 0.0 &&
      (sel.clipIndex >= 0 || sel.midiRegion);
  ExportDialog dialog(
      misuc::audio::ExportFormat::WavFloat, loopAvailable, selectionAvailable,
      this, QString::fromStdString(session_.name()),
      QString::fromStdString(session_.artist()), session_.sampleRate());
  if (dialog.exec() != QDialog::Accepted)
    return;

  const QString home = projectPath_.isEmpty()
                           ? QDir::homePath()
                           : QFileInfo(projectPath_).absolutePath();
  const QString base = projectPath_.isEmpty()
                           ? QStringLiteral("untitled")
                           : QFileInfo(projectPath_).completeBaseName();
  const QString suffix = dialog.suffix();
  // Build a filter list from the codecs actually available on this system.
  QStringList filters;
  const auto kFormats = misuc::audio::availableExportFormats();
  for (const auto f : kFormats) {
    const QString ext =
        QString::fromLatin1(misuc::audio::exportFormatSuffix(f));
    const QString label =
        f == misuc::audio::ExportFormat::WavFloat   ? QStringLiteral("WAV (32-bit float)")
        : f == misuc::audio::ExportFormat::WavPcm16 ? QStringLiteral("WAV (16-bit PCM)")
        : f == misuc::audio::ExportFormat::Flac     ? QStringLiteral("FLAC (lossless)")
        : f == misuc::audio::ExportFormat::OggVorbis ? QStringLiteral("Ogg Vorbis")
        : f == misuc::audio::ExportFormat::Mp3      ? QStringLiteral("MP3 (Layer III)")
        : QStringLiteral("AIFF");
    filters << QStringLiteral("%1 (*.%2)").arg(label, ext);
  }
  QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Export audio"),
      QDir(home).filePath(base + QStringLiteral("-export.") + suffix),
      filters.join(QLatin1Char(';')));
  if (path.isEmpty())
    return;
  if (!path.endsWith(QStringLiteral(".") + suffix, Qt::CaseInsensitive))
    path += QStringLiteral(".") + suffix;

  doExport(path, dialog.format(), static_cast<int>(dialog.range()),
           dialog.dither(), dialog.title(), dialog.artist(),
           dialog.sampleRate(), dialog.includeMetronome());
}

void MainWindow::doExport(const QString& path,
                          misuc::audio::ExportFormat format, int rangeKind,
                          bool dither, const QString& title,
                          const QString& artist, uint32_t exportSampleRate,
                          bool includeMetronome) {
  // Offline bounce must not race the live stream: pause the transport, stop
  // the device, render to disk, then restore the stream we stopped.
  const bool wasRunning = engine_.isRunning();
  const bool wasPlaying = engine_.isTransportPlaying();
  if (wasRunning) {
    if (wasPlaying)
      engine_.pause();
    engine_.closeAudio();
  }
  auto restore = [this, wasRunning] {
    if (wasRunning)
      audioSettings_->restartStream();
  };

  // Bounce the extent that contains all arranged material (notes, clips, loop
  // region), with a minimum of one bar -- the whole-song baseline.
  double endBeats = 4.0;
  for (const auto& tr : session_.tracks()) {
    for (const auto& n : tr.notes)
      endBeats = std::max(endBeats, n.startBeats + n.durationBeats);
    for (const auto& c : tr.clips)
      endBeats = std::max(endBeats, c.startBeats + c.durationBeats);
  }
  if (session_.loopMode())
    endBeats = std::max(endBeats, session_.loopEnd());

  double fromBeats = 0.0, toBeats = endBeats;
  if (rangeKind == 1 && session_.loopMode() &&
      session_.loopEnd() > session_.loopStart()) {
    fromBeats = session_.loopStart();
    toBeats = session_.loopEnd();
  } else if (rangeKind == 2) {
    const auto sel = playlist_ ? playlist_->selectionSummary()
                               : Playlist::SelectionSummary{};
    if (sel.durationBeats > 0.0 && (sel.clipIndex >= 0 || sel.midiRegion)) {
      fromBeats = sel.startBeats;
      toBeats = std::max(sel.startBeats + sel.durationBeats, fromBeats + 0.5);
    }
  }
  if (toBeats <= fromBeats) {
    restore();
    QMessageBox::warning(this, QStringLiteral("Export failed"),
                         QStringLiteral("Nothing to export in that range."));
    return;
  }

  std::string err;
  const double totalBeats = toBeats - fromBeats;
  // Progress dialog (protocol #13 §11): filename, elapsed time, progress
  // bar, and a Cancel that aborts the offline render before the file is
  // written (a cancelled export never leaves a partial/misleading file).
  QProgressDialog progress(QString(), QStringLiteral("Cancel"), 0, 1000, this);
  progress.setWindowTitle(QStringLiteral("Exporting"));
  progress.setWindowModality(Qt::WindowModal);
  progress.setObjectName(QStringLiteral("exportProgressDialog"));
  progress.setMinimumDuration(0);
  progress.setAutoClose(true);
  progress.setAutoReset(false);
  progress.setCancelButtonText(QStringLiteral("Cancel"));
  QElapsedTimer elapsed;
  elapsed.start();
  const QString fileName = QFileInfo(path).fileName();
  auto updateLabel = [&](double frac) {
    progress.setValue(static_cast<int>(frac * 1000.0));
    const qint64 ms = elapsed.elapsed();
    progress.setLabelText(
        QStringLiteral("Rendering %1...\n%2 s elapsed")
            .arg(fileName)
            .arg(static_cast<double>(ms) / 1000.0, 0, 'f', 1));
  };
  updateLabel(0.0);

  const bool ok = engine_.renderToFile(
      path.toStdString(), fromBeats, toBeats, 2, format, dither, err,
      exportSampleRate, title.toStdString(), artist.toStdString(),
      includeMetronome, [&](double frac) {
        if (frac >= 0.0) {
          // Drive the modal dialog's event loop so Cancel stays responsive.
          QCoreApplication::processEvents(
              QEventLoop::ExcludeUserInputEvents, 16);
          updateLabel(frac);
        }
        return !progress.wasCanceled();
      });

  progress.setValue(1000);
  restore();
  if (!ok) {
    if (progress.wasCanceled())
      statusBar()->showMessage(
          QStringLiteral("Export cancelled: %1").arg(fileName), 5000);
    else
      QMessageBox::warning(this, QStringLiteral("Export failed"),
                           QString::fromStdString(err));
    return;
  }
  statusBar()->showMessage(
      QStringLiteral("Exported %1 (%2 beats)").arg(path).arg(
          totalBeats, 0, 'f', 1));
}

QString MainWindow::stageAudioFile(const QString& src) const {
  if (projectPath_.isEmpty())
    return src; // no project on disk yet: reference the original file
  QFileInfo si(src);
  if (!si.exists())
    return src;
  const QString dir =
      QDir(QFileInfo(projectPath_).absolutePath()).filePath(QStringLiteral("audio"));
  if (!QDir().mkpath(dir))
    return src;
  const QString dst = QDir(dir).filePath(si.fileName());
  if (QFileInfo(dst).exists() || QFile::copy(src, dst))
    return dst;
  return src;
}

void MainWindow::poll() {
  headerBar_->refresh();
  status_->refresh();
  feedPlayhead();
  mixerPage_->pollMeters();
  hints_->refresh();
}

void MainWindow::feedPlayhead() {
  // Transport position (frames) -> beats via current tempo, and drive the
  // piano-roll caret with auto-follow from the same 30 Hz UI tick.
  const double bpm = session_.tempo();
  if (bpm <= 0.0)
    return;
  const uint64_t frames = engine_.playheadSamples();
  const double beats =
      static_cast<double>(frames) / ((60.0 / bpm) * engine_.sampleRate());
  editorPage_->piano()->setPlayhead(engine_.isTransportPlaying(), beats);
  if (playlist_)
    playlist_->feedPlayhead(engine_.isTransportPlaying(), beats);
}

void MainWindow::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), theme::kBackground);
}

void MainWindow::togglePlayStop() {
  if (engine_.isTransportPlaying())
    engine_.pause();
  else
    engine_.play(false);
}

void MainWindow::applyLayoutPreset(const QString& name) {
  wsMan_->applyPreset(name);
}

void MainWindow::syncLayoutActions() {
  const QString current = wsMan_->presetName();
  for (QAction* a : presetActions_)
    a->setChecked(a->text() == current);
}

void MainWindow::buildWorkspaceViewMenu() {
  if (!viewMenu_ || !wsMan_)
    return;
  viewMenu_->addSeparator();
  // Panel visibility toggles: every major workspace panel is listed, carries
  // its keyboard shortcut, is checkable, and reflects the panel's real state
  // (docks: shown/hidden; central editors: active tab). Checking/unchecking
  // hides or shows the panel (central editors switch between each other).
  const struct {
    workspaces::Id id;
    QLatin1String label;
    QKeySequence key;
  } panels[] = {
      {workspaces::Id::Playlist,    QLatin1String("Playlist"),        QKeySequence(Qt::Key_F5)},
      {workspaces::Id::PianoRoll,   QLatin1String("Piano Roll"),      QKeySequence(Qt::Key_F7)},
      {workspaces::Id::ChannelRack, QLatin1String("Channel Rack"),    QKeySequence(Qt::Key_F6)},
      {workspaces::Id::Browser,     QLatin1String("Browser"),         QKeySequence(Qt::Key_F8)},
      {workspaces::Id::Mixer,       QLatin1String("Mixer"),           QKeySequence(Qt::Key_F9)},
      {workspaces::Id::Inspector,   QLatin1String("Inspector"),       QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I)},
  };
  for (const auto& p : panels) {
    const bool dock = (p.id == workspaces::Id::ChannelRack ||
                       p.id == workspaces::Id::Mixer ||
                       p.id == workspaces::Id::Browser ||
                       p.id == workspaces::Id::Inspector);
    QAction* act = viewMenu_->addAction(p.label, this,
                                        [this, p, dock] {
                                          if (dock) {
                                            wsMan_->toggleWorkspace(p.id);
                                          } else {
                                            // Central tabs cannot be hidden;
                                            // toggling one off moves to its
                                            // sibling editor.
                                            const workspaces::Id other =
                                                p.id == workspaces::Id::Playlist
                                                    ? workspaces::Id::PianoRoll
                                                    : workspaces::Id::Playlist;
                                            wsMan_->showWorkspace(
                                                wsMan_->isVisible(p.id) ? other
                                                                        : p.id,
                                                true);
                                          }
                                        });
    act->setCheckable(true);
    act->setShortcut(p.key);
    act->setChecked(false);
    wsPanelActions_.append({p.id, act});
  }
  // Keep the toggles honest with the real workspace state.
  connect(wsMan_, &WorkspaceManager::layoutChanged, this,
          &MainWindow::syncWsPanelActions);
  connect(wsMan_, &WorkspaceManager::workspaceToggled, this,
          &MainWindow::syncWsPanelActions);
  connect(wsMan_, &WorkspaceManager::centralChanged, this,
          &MainWindow::syncWsPanelActions);
  syncWsPanelActions();
}

void MainWindow::syncWsPanelActions() {
  if (!wsMan_ || wsMan_->isTearingDown())
    return;
  for (const auto& pair : std::as_const(wsPanelActions_)) {
    const workspaces::Id id = pair.first;
    QAction* act = pair.second;
    if (!act)
      continue;
    QDockWidget* dock = wsMan_->docks(id);
    bool on = false;
    if (dock) {
      on = !dock->isHidden();
    } else if (workspace_ && wsMan_->widget(id)) {
      on = workspace_->currentWidget() == wsMan_->widget(id);
    }
    act->setChecked(on);
  }
}

void MainWindow::syncWorkspaceStrip() {
  const workspaces::Id ids[] = {workspaces::Id::Playlist,
                                workspaces::Id::PianoRoll,
                                workspaces::Id::ChannelRack,
                                workspaces::Id::Mixer,
                                workspaces::Id::Browser,
                                workspaces::Id::Inspector};
  for (workspaces::Id id : ids)
    headerBar_->setWorkspaceActive(wsStripId(id), wsMan_->isVisible(id));
}

void MainWindow::persistLayout() {
  wsMan_->saveLayoutState();
  QSettings settings(kSettingsOrg, kSettingsApp);
  settings.setValue(QStringLiteral("workspace/size"), size());
}

void MainWindow::restoreLayout() {
  wsMan_->restoreLayoutState();
  QSettings settings(kSettingsOrg, kSettingsApp);
  const QVariant size = settings.value(QStringLiteral("workspace/size"));
  const QSize saved = size.toSize();
  if (saved.isValid() && !saved.isEmpty())
    resize(saved);
}

void MainWindow::onInspectorEdit(const QString& property,
                                 const QString& value) {
  if (!playlist_) return;
  auto sel = playlist_->selectionSummary();
  if (sel.trackRow < 0) return;
  auto* tr = session_.findTrack(sel.trackId.toStdString());
  if (!tr) return;

  if (property == "track.name") {
    tr->name = value.toStdString();
  } else if (property == "track.mute") {
    tr->muted = value == "1";
  } else if (property == "track.solo") {
    tr->solo = value == "1";
  } else if (property == "track.gain") {
    tr->gainDb = value.toFloat();
  } else if (property == "track.pan") {
    tr->pan = qBound(-1.0f, value.toFloat(), 1.0f);
  } else if (property == "fx.add") {
    // "fx.add" carries the plugin URI to insert at the end of the chain.
    // Session::addFx appends a slot (params warmed at hosting time).
    const std::string uri = value.toStdString();
    if (!uri.empty())
      session_.addFx(tr->id, uri);
  } else if (property.startsWith("fx.")) {
    // fx.<id>.enable / fx.<id>.remove / fx.<id>.param.<port>
    QStringList parts = property.split('.');
    if (parts.size() < 3 || parts[1].isEmpty())
      return;
    const std::string fxId = parts[1].toStdString();
    misuc::project::TrackFx* fxSlot = nullptr;
    size_t chainIdx = tr->fx.size();
    for (size_t ii = 0; ii < tr->fx.size(); ++ii)
      if (tr->fx[ii].id == fxId) {
        fxSlot = &tr->fx[ii];
        chainIdx = ii;
      }
    if (parts[2] == "enable") {
      if (fxSlot) fxSlot->enabled = value == "1";
    } else if (parts[2] == "remove") {
      session_.removeFx(tr->id, fxId);
    } else if (parts[2] == "param" && parts.size() == 4 && fxSlot) {
      const uint32_t port = parts[3].toUInt();
      const float v = value.toFloat();
      bool found = false;
      for (auto& p : fxSlot->params)
        if (p.index == port) {
          p.value = v;
          found = true;
          break;
        }
      if (!found)
        fxSlot->params.push_back({port, v});
    } else if (parts[2] == "auto" && parts.size() == 4 && fxSlot) {
      // Arm/unarm fx control-port automation for the targeted insert: the "A"
      // button in the inspector toggles the "fx:<chain>:<port>" lane. Arming
      // creates the lane first (resting at the current control value) so the
      // playlist band immediately shows a recordable curve.
      const uint32_t port = parts[3].toUInt();
      const bool want = value == "1";
      const std::string lp =
          "fx:" + std::to_string(chainIdx) + ":" + std::to_string(port);
      auto* al = static_cast<misuc::project::AutomationLane*>(nullptr);
      for (auto& lane : tr->automation)
        if (lane.param == lp)
          al = &lane;
      if (want) {
        if (!al) {
          double rest = 0.0;
          if (const misuc::engine::FxPluginInfo* info =
                  engine_.fxInfo(fxSlot->uri))
            for (const auto& pi : info->params)
              if (pi.index == port)
                rest = pi.def;
          for (const auto& p : fxSlot->params)
            if (p.index == port)
              rest = p.value;
          session_.addAutomationLane(tr->id, lp, rest);
          for (auto& lane : tr->automation)
            if (lane.param == lp)
              al = &lane;
        }
        if (al) al->armed = true;
      } else if (al) {
        al->armed = false;
      }
    } else {
      return;
    }
  } else if (property.startsWith("take.")) {
    // take.<takeId>.place / take.<takeId>.remove
    QStringList parts = property.split('.');
    if (parts.size() != 3 || parts[1].isEmpty())
      return;
    const std::string takeId = parts[1].toStdString();
    misuc::project::RecordTake* take = session_.findTake(tr->id, takeId);
    if (!take)
      return;
    if (parts[2] == "place") {
      placeTake(*tr, *take);
    } else if (parts[2] == "remove") {
      // Delete the take and (if it was placed) take its clip off the timeline.
      if (!take->clipId.empty()) {
        for (auto it = tr->clips.begin(); it != tr->clips.end(); ++it) {
          if (it->id == take->clipId) {
            tr->clips.erase(it);
            break;
          }
        }
      }
      session_.removeTake(tr->id, takeId);
    } else {
      return;
    }
  } else if (property.startsWith("clip.")) {
    if (sel.clipIndex < 0 ||
        sel.clipIndex >= static_cast<int>(tr->clips.size()))
      return;
    auto& clip = tr->clips[static_cast<size_t>(sel.clipIndex)];
    if (property == "clip.name") {
      clip.name = value.toStdString();
    } else if (property == "clip.gain") {
      clip.gainDb = value.toFloat();
    } else if (property == "clip.trim") {
      clip.clipOffsetBeats = value.toDouble();
    } else {
      return;
    }
  } else {
    return;
  }
  onSessionEdited();
}

void MainWindow::refreshInspector() {
  if (!inspector_ || !playlist_) return;
  auto sel = playlist_->selectionSummary();
  if (sel.trackRow < 0) {
    inspector_->showEmpty(
        QStringLiteral("Select a track, clip or MIDI region in the Playlist to "
                        "inspect it."));
    return;
  }
  if (sel.clipIndex >= 0 && !sel.midiRegion) {
    inspector_->showAudioClip(sel.clipName, sel.clipSource, sel.startBeats,
                              sel.durationBeats, sel.gainDb,
                              sel.clipOffsetBeats);
    return;
  }
  if (sel.midiRegion) {
    inspector_->showMidiRegion(sel.noteCount, sel.startBeats,
                               sel.durationBeats);
    return;
  }
  auto* tr = session_.findTrack(sel.trackId.toStdString());
  if (!tr) {
    inspector_->clear();
    return;
  }
  inspector_->showTrack(sel.trackRow, sel.trackName, sel.trackMuted,
                        sel.trackSolo, sel.trackGainDb, sel.trackPan,
                        buildFxSlots(*tr), buildFxCatalog(),
                        buildTakeViews(*tr));
}

std::vector<misuc::ui::TakeView> MainWindow::buildTakeViews(
    const misuc::project::Track& tr) {
  std::vector<misuc::ui::TakeView> result;
  for (const auto& k : tr.takes) {
    result.push_back(misuc::ui::TakeView{
        QString::fromStdString(k.id), QString::fromStdString(k.name),
        k.startBeats, k.durationBeats,
        !k.clipId.empty()}); // placed while its clip is on the timeline
  }
  return result;
}

std::vector<misuc::ui::FxCatalogItem> MainWindow::buildFxCatalog() {
  std::vector<misuc::ui::FxCatalogItem> catalog;
  for (const auto& p : engine_.discoverFx())
    catalog.push_back({QString::fromStdString(p.uri),
                       QString::fromStdString(p.name)});
  return catalog;
}

std::vector<misuc::ui::FxSlotView> MainWindow::buildFxSlots(
    const misuc::project::Track& tr) {
  std::vector<misuc::ui::FxSlotView> result;
  size_t chainIdx = 0;
  for (const auto& f : tr.fx) {
    misuc::ui::FxSlotView sv;
    sv.id = QString::fromStdString(f.id);
    sv.enabled = f.enabled;
    sv.name = QString::fromStdString(f.uri);
    if (const misuc::engine::FxPluginInfo* info = engine_.fxInfo(f.uri)) {
      sv.name = QString::fromStdString(info->name);
      for (const auto& pi : info->params) {
        misuc::ui::FxSlotView::Param pa;
        pa.index = pi.index;
        pa.label = QString::fromStdString(pi.label);
        pa.min = pi.min;
        pa.max = pi.max;
        pa.value = pi.def;
        for (const auto& v : f.params)
          if (v.index == pi.index) {
            pa.value = v.value;
            break;
          }
        // "A" arm state: reflects a non-muted "fx:<chain>:<index>" lane.
        const std::string lp = "fx:" + std::to_string(chainIdx) + ":" +
                               std::to_string(pi.index);
        for (const auto& lane : tr.automation)
          if (lane.param == lp && !lane.muted) {
            pa.armed = lane.armed;
            break;
          }
        sv.params.push_back(std::move(pa));
      }
    }
    result.push_back(std::move(sv));
    ++chainIdx;
  }
  return result;
}

void MainWindow::buildCommands() {
  using core::MisucCommandRegistry;
  MisucCommandRegistry& reg = MisucCommandRegistry::instance();
  const auto mo = [](const char* t) { return QString::fromLatin1(t); };

  QKeySequence f5(Qt::Key_F5), f6(Qt::Key_F6), f7(Qt::Key_F7), f8(Qt::Key_F8),
      f9(Qt::Key_F9);
  auto openWs = [this](workspaces::Id ws) {
    return [this, ws] { wsMan_->showWorkspace(ws, true); };
  };

  reg.registerCommand({mo("ws.playlist"), mo("Playlist"),
      mo("Show the arrangement workspace (audio clips + MIDI regions)"),
      mo("Workspace"), icons::IconId::Playlist, f5, nullptr, nullptr, nullptr,
      openWs(workspaces::Id::Playlist)});
  reg.registerCommand({mo("ws.pianoroll"), mo("Piano Roll"),
      mo("Show the MIDI editor for the selected track"), mo("Workspace"),
      icons::IconId::PianoRoll, f7, nullptr, nullptr, nullptr,
      openWs(workspaces::Id::PianoRoll)});
  reg.registerCommand({mo("ws.channelrack"), mo("Channel Rack"),
      mo("Show the step-sequencer channel rack"), mo("Workspace"),
      icons::IconId::ChannelRack, f6, nullptr, nullptr, nullptr,
      openWs(workspaces::Id::ChannelRack)});
  reg.registerCommand({mo("ws.mixer"), mo("Mixer"),
      mo("Show per-track gain/pan/mute/solo and meters"), mo("Workspace"),
      icons::IconId::Mixer, f9, nullptr, nullptr, nullptr,
      openWs(workspaces::Id::Mixer)});
  reg.registerCommand({mo("ws.browser"), mo("Browser"),
      mo("Browse samples, project audio, instruments and effects"),
      mo("Workspace"), icons::IconId::Browser, f8, nullptr, nullptr, nullptr,
      openWs(workspaces::Id::Browser)});
  reg.registerCommand({mo("ws.inspector"), mo("Inspector"),
      mo("Inspect and edit the current selection"), mo("Workspace"),
      icons::IconId::Inspector, QKeySequence(QStringLiteral("Ctrl+Shift+I")),
      nullptr, nullptr, nullptr, openWs(workspaces::Id::Inspector)});

  reg.registerCommand({mo("transport.play"), mo("Play / Stop"),
      mo("Start or stop the transport from the playhead"), mo("Transport"),
      icons::IconId::Play, QKeySequence(Qt::Key_Space), nullptr, nullptr,
      nullptr, [this] { togglePlayStop(); }});
  reg.registerCommand({mo("transport.record"), mo("Record"),
      mo("Arm / disarm clip recording"), mo("Transport"), icons::IconId::Record,
      QKeySequence(Qt::Key_R), nullptr,
      [this] { return !engine_.isRecording(); }, nullptr,
      [this] {
        // Delegate to the Record Setup dialog workflow
        onRecordRequested();
      }});
  reg.registerCommand({mo("transport.metronome"), mo("Metronome"),
      mo("Toggle the click and count-in lead-in"), mo("Transport"),
      icons::IconId::Metronome, QKeySequence(Qt::Key_M), nullptr,
      [this] { return metronomeCmd_ && !metronomeCmd_->isChecked(); },
      [this] {
        return metronomeCmd_ && metronomeCmd_->isChecked()
                   ? QStringLiteral("ON")
                   : QStringLiteral("OFF");
      },
      [this] {
        bool on = !(metronomeCmd_ && metronomeCmd_->isChecked());
        metronomeCmd_->setChecked(on);
        engine_.setMetronome(on, 2.f);
      }});
  reg.registerCommand({mo("transport.snap"), mo("Snap to Grid"),
      mo("Quantize clip and note edits to the grid"), mo("Edit"),
      icons::IconId::Snap, QKeySequence(Qt::Key_N), nullptr,
      [this] { return transport_ && transport_->snapOn(); },
      [this] {
        return transport_ && transport_->snapOn() ? QStringLiteral("ON")
                                                  : QStringLiteral("OFF");
      },
      [this] {
        bool on = transport_->snapOn() ? false : true;
        transport_->setSnapOn(on);
        if (playlist_) playlist_->setSnap(on);
      }});
  reg.registerCommand({mo("mode.song"), mo("Song Mode"),
      mo("Arrange on the song timeline"), mo("Mode"), icons::IconId::SongMode,
      QKeySequence(), nullptr, nullptr, nullptr, [this] {
        transport_->setSongMode(true);
        if (playlist_) playlist_->setSongMode(true);
      }});
  reg.registerCommand({mo("mode.pattern"), mo("Pattern Mode"),
      mo("Work on the current pattern (channel rack)"), mo("Mode"),
      icons::IconId::PatternMode, QKeySequence(), nullptr, nullptr, nullptr,
      [this] {
        transport_->setSongMode(false);
        if (playlist_) playlist_->setSongMode(false);
      }});
}

void MainWindow::showAbout() {
  misuc::ui::AboutDialog dlg(this);
  dlg.exec();
}

void MainWindow::showUserGuide() {
  QFile guide(QStringLiteral(":/help/user-guide.txt"));
  QString text;
  if (guide.open(QIODevice::ReadOnly | QIODevice::Text))
    text = QString::fromUtf8(guide.readAll());
  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("Misuc User Guide"));
  dlg.resize(560, 620);
  auto* lay = new QVBoxLayout(&dlg);
  auto* view = new QPlainTextEdit(&dlg);
  view->setReadOnly(true);
  view->setPlainText(text);
  view->setFont(QFont(QStringLiteral("monospace"), 10));
  lay->addWidget(view);
  dlg.exec();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!confirmDiscard()) {
    event->ignore();
    return;
  }
  // Re-dock any floating panels before the window is destroyed so shutdown
  // never leaves orphan top-level float windows behind (clean teardown).
  if (wsMan_)
    wsMan_->settleFloatingDocks();
  if (workspace_)
    persistLayout();
  clearAutosave(); // closing discarded it (saving already cleared it)
  event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (event->mimeData() && event->mimeData()->hasUrls()) {
    const auto urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
      if (url.isLocalFile() &&
          url.toLocalFile().endsWith(QStringLiteral(".misuc"),
                                     Qt::CaseInsensitive)) {
        event->acceptProposedAction();
        return;
      }
    }
  }
  event->ignore();
}

void MainWindow::dropEvent(QDropEvent* event) {
  if (!event->mimeData()) {
    event->ignore();
    return;
  }
  const auto urls = event->mimeData()->urls();
  for (const QUrl& url : urls) {
    if (!url.isLocalFile() ||
        !url.toLocalFile().endsWith(QStringLiteral(".misuc"),
                                    Qt::CaseInsensitive))
      continue;
    if (!confirmDiscard())
      return;
    openProject(url.toLocalFile());
    event->acceptProposedAction();
    return;
  }
  event->ignore();
}

void MainWindow::newProject() {
  if (!confirmDiscard())
    return;
  session_ = misuc::project::Session();
  projectPath_.clear();
  dirty_ = false;
  undo_.clear();
  undo_.push(session_);
  clearAutosave();
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  channelRack_->refresh();
  applyTempoToEngine();
  syncLoopState(); // fresh document: loop off everywhere
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  undoCmd_->setEnabled(false);
  redoCmd_->setEnabled(false);
  updateWindowTitle();
}

bool MainWindow::importFlpFromPath(const QString& path,
                                   misuc::flp::FlpImportResult* outResult) {
  const std::string src = path.toStdString();
  misuc::flp::FlpResult flp = misuc::flp::FlpReader::parseFile(src);
  if (!flp.ok) return false;
  misuc::flp::FlpImportResult res =
      misuc::flp::importSession(flp, QFileInfo(path).absolutePath().toStdString());
  session_ = std::move(res.session);
  session_.resolveAssets(QFileInfo(path).absolutePath().toStdString());
  projectPath_.clear(); // user must Save As to persist the imported song
  dirty_ = true;        // imported work is a real document; autosave it
  undo_.clear();
  undo_.push(session_);
  clearAutosave();
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  channelRack_->refresh();
  applyTempoToEngine();
  syncLoopState();
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  applyFxToEngine();
  undoCmd_->setEnabled(false);
  redoCmd_->setEnabled(false);
  updateWindowTitle();
  if (outResult) *outResult = std::move(res);
  return true;
}

void MainWindow::importFlp() {
  if (!confirmDiscard())
    return;
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Import FL Studio project"), QString(),
      QStringLiteral("FL Studio project (*.flp);;All files (*)"));
  if (path.isEmpty())
    return;
  misuc::flp::FlpImportResult res;
  if (!importFlpFromPath(path, &res))
    return;
  const QString flVersion = [&path] {
    misuc::flp::FlpResult p =
        misuc::flp::FlpReader::parseFile(path.toStdString());
    return p.ok ? QString::fromLatin1(p.flVersion.c_str())
                : QStringLiteral("?");
  }();
  QString detail =
      QStringLiteral("Imported %1\n(FLP v%2, %3 channel(s), %4 track(s), "
                     "%5 MIDI region(s))")
          .arg(QFileInfo(path).fileName())
          .arg(flVersion)
          .arg(res.channelsImported)
          .arg(session_.tracks().size())
          .arg(res.playlistRegions);
  detail += QStringLiteral("\n\n  %1 note(s) placed in %2 region(s); "
                           "%3 pattern(s) on the timeline")
                .arg(res.notesImported)
                .arg(res.playlistRegions)
                .arg(res.patternsImported);
  if (res.samplerChannels > 0 || res.samplesFound > 0 ||
      res.samplesMissing > 0) {
    detail += QStringLiteral("\n  %1 sampler channel(s); %2 sample(s) found, "
                             "%3 missing")
                  .arg(res.samplerChannels)
                  .arg(res.samplesFound)
                  .arg(res.samplesMissing);
  }
  if (res.unsupportedPlugins > 0) {
    detail += QStringLiteral("\n  %1 FL plugin(s) not hosted "
                             "(placeholder rows)")
                  .arg(res.unsupportedPlugins);
  }
  if (!res.warnings.empty()) {
    QString w;
    for (size_t i = 0; i < res.warnings.size() && i < 8; ++i) {
      w += QStringLiteral("\n• ") +
           QString::fromLatin1(res.warnings[i].c_str());
    }
    if (res.warnings.size() > 8)
      w += QStringLiteral("\n… %1 more").arg(res.warnings.size() - 8);
    detail +=
        QStringLiteral("\n\nNot imported (%1):").arg(res.warnings.size()) + w;
  }
  QMessageBox::information(this, QStringLiteral("FLP imported"), detail);
}

void MainWindow::openDialog() {
  if (!confirmDiscard())
    return;
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Open Misuc project"), QString(),
      QStringLiteral("Misuc project (*.misuc)"));
  if (!path.isEmpty())
    openProject(path);
}

void MainWindow::openDemoProject() {
  if (!confirmDiscard())
    return;
  DemoPaths paths;
  QString err;
  if (!ensureDemoProject(paths, err)) {
    QMessageBox::warning(this, QStringLiteral("Demo unavailable"), err);
    return;
  }
  openProject(paths.project);
}

void MainWindow::openProject(const QString& path) {
  std::string json, err;
  if (!readProjectDocument(path, json, &err)) {
    QMessageBox::warning(this, QStringLiteral("Open failed"),
                         QString::fromStdString(err));
    return;
  }
  try {
    session_ = misuc::project::Session::fromJson(json);
  } catch (const std::exception& e) {
    QMessageBox::warning(this, QStringLiteral("Invalid project"),
                         QString::fromLatin1(e.what()));
    return;
  }
  // Saved documents store assets inside the project folder as relative paths;
  // the running session needs absolute paths so the engine can play them.
  session_.resolveAssets(QFileInfo(path).absolutePath().toStdString());
  projectPath_ = path;
  dirty_ = false;
  undo_.clear();
  undo_.push(session_);
  clearAutosave(); // an opened document supersedes any recovery copy
  addRecentProject(path);
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  channelRack_->refresh();
  applyTempoToEngine();
  syncLoopState();
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  undoCmd_->setEnabled(false);
  redoCmd_->setEnabled(false);
  updateWindowTitle();
}

QStringList MainWindow::recentProjectsFromSettings() const {
  QSettings settings(kSettingsOrg, kSettingsApp);
  const QStringList list =
      settings.value(kRecentProjectsKey).toStringList();
  QStringList out;
  for (const QString& p : list) {
    // Dedupe only. Keep entries whose file has since disappeared: the Open
    // Recent menu marks them unavailable and offers removal on click
    // (protocol #7 Part 2, §Missing files).
    if (out.contains(p))
      continue;
    out << p;
  }
  return out;
}

QStringList MainWindow::recentProjects() const {
  return recentProjectsFromSettings();
}

void MainWindow::applyAutosaveInterval(int seconds) {
  autosaveIntervalSecs_ = std::clamp(seconds, 0, 24 * 3600);
  if (autosaveIntervalSecs_ <= 0) {
    autosaveTimer_->stop();
    return;
  }
  autosaveTimer_->setInterval(autosaveIntervalSecs_ * 1000);
  autosaveTimer_->start();
}

void MainWindow::setAutosaveIntervalSeconds(int seconds) {
  applyAutosaveInterval(seconds);
  QSettings settings(kSettingsOrg, kSettingsApp);
  settings.setValue(kAutosaveIntervalKey, autosaveIntervalSecs_);
}

void MainWindow::addRecentProject(const QString& path) {
  QSettings settings(kSettingsOrg, kSettingsApp);
  const QString abs = QFileInfo(path).absoluteFilePath();
  QStringList list = settings.value(kRecentProjectsKey).toStringList();
  list.removeAll(abs);
  list.prepend(abs);
  while (list.size() > 12)
    list.removeLast();
  settings.setValue(kRecentProjectsKey, list);
  rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
  recentMenu_->clear();
  const QStringList recents = recentProjects();
  if (recents.isEmpty()) {
    QAction* none = recentMenu_->addAction(QStringLiteral("(no recent projects)"));
    none->setEnabled(false);
    return;
  }
  // File names that appear more than once need path context to disambiguate
  // (protocol #7 Part 2: show path info if names collide).
  QSet<QString> dupNames;
  {
    QSet<QString> seen;
    for (const QString& path : recents) {
      const QString name = QFileInfo(path).fileName();
      if (seen.contains(name))
        dupNames.insert(name);
      seen.insert(name);
    }
  }
  const QString home = QDir::homePath();
  const auto displayPath = [&home](const QString& path) {
    if (path.startsWith(home))
      return QStringLiteral("~") + path.mid(home.size());
    return path;
  };
  for (const QString& path : recents) {
    const bool missing = !QFileInfo(path).exists();
    const QString name = QFileInfo(path).fileName();
    QString label = dupNames.contains(name) ? displayPath(path) : name;
    if (missing)
      label += QStringLiteral(" — unavailable");
    if (missing) {
      QAction* act = recentMenu_->addAction(label);
      act->setToolTip(path);
      act->setEnabled(true);
      connect(act, &QAction::triggered, this, [this, path] {
        // Confirmation before dropping a dead entry (protocol: "visually mark
        // it unavailable, or remove it after user confirmation/use").
        QMessageBox box(QMessageBox::Warning,
                        QStringLiteral("Recent project unavailable"),
                        QStringLiteral("This recent project no longer "
                                       "exists:\n%1\n\nRemove it from the "
                                       "recent list?")
                            .arg(path),
                        QMessageBox::NoButton, this);
        QPushButton* removeBtn =
            box.addButton(QStringLiteral("&Remove"), QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("&Keep"), QMessageBox::RejectRole);
        box.setDefaultButton(removeBtn);
        box.exec();
        if (box.clickedButton() != removeBtn)
          return;
        QSettings settings(kSettingsOrg, kSettingsApp);
        QStringList list = settings.value(kRecentProjectsKey).toStringList();
        list.removeAll(path);
        settings.setValue(kRecentProjectsKey, list);
        rebuildRecentMenu();
      });
    } else {
      recentMenu_->addAction(label, this, [this, path] {
        if (!confirmDiscard())
          return;
        openProject(path);
      });
    }
  }
  recentMenu_->addSeparator();
  recentMenu_->addAction(QStringLiteral("Clear Recent"), this, [this] {
    QSettings settings(kSettingsOrg, kSettingsApp);
    settings.remove(kRecentProjectsKey);
    rebuildRecentMenu();
  });
}

bool MainWindow::saveProject() {
  if (projectPath_.isEmpty())
    return saveProjectAs();
  return writeProject(projectPath_);
}

bool MainWindow::saveProjectAs() {
  const QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Save Misuc project"), projectPath_,
      QStringLiteral("Misuc project (*.misuc)"));
  if (path.isEmpty())
    return false;
  return writeProject(path);
}

bool MainWindow::writeProject(const QString& path) {
  // Stamp the live document, then serialize a relativized copy so the
  // document is portable alongside its `<folder>/audio/` assets; the live
  // session keeps absolute paths.
  session_.ensureCreatedUtc(); // preserve the document's first-save birthday
  session_.touchModified();
  misuc::project::Session copy = session_;
  copy.relativizeAssets(QFileInfo(path).absolutePath().toStdString());
  // Atomic write of the native container: temp file + fsync + rename, so a
  // crash mid-save can never truncate the existing document on disk.
  std::string err;
  if (!writeProjectDocument(path, copy.toJson(), &err)) {
    QMessageBox::warning(this, QStringLiteral("Save failed"),
                         QString::fromStdString(err));
    return false;
  }
  projectPath_ = path;
  dirty_ = false;
  clearAutosave(); // a real save supersedes any crash-recovery copy
  addRecentProject(path);
  updateWindowTitle();
  return true;
}

QString MainWindow::autosavePath() {
  const QString dir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return dir + QStringLiteral("/autosave.misuc");
}

QString MainWindow::autosavePrevPath() {
  const QString dir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return dir + QStringLiteral("/autosave.prev.misuc");
}

void MainWindow::clearAutosave() {
  QFile::remove(autosavePath());
  QFile::remove(autosavePrevPath());
}

void MainWindow::autosaveNow() {
  if (!dirty_ || session_.contentEndBeats() <= 0.0)
    return;
  const QString dir = QFileInfo(autosavePath()).absolutePath();
  QDir d(dir);
  if (!d.exists())
    d.mkpath(QStringLiteral("."));

  const QString cur = autosavePath();
  const QString prev = autosavePrevPath();

  // Rotate: the current autosave becomes the previous one (dropping its own
  // predecessor). A corrupt current copy is dropped rather than promoted so
  // the recovery path always has at least the last good session.
  {
    std::string json, err;
    bool curGood = readProjectDocument(cur, json, &err);
    if (curGood) {
      try {
        (void)misuc::project::Session::fromJson(json);
      } catch (const std::exception&) {
        curGood = false;
      }
    }
    if (curGood) {
      QFile::remove(prev);
      QFile::rename(cur, prev);
    } else {
      QFile::remove(cur);
    }
  }

  session_.ensureCreatedUtc();
  session_.touchModified();
  std::string err;
  if (!writeProjectDocument(cur, session_.toJson(), &err))
    return; // keep .prev as the best surviving copy

  // Verify the write landed as a readable session; never leave a recognized
  // autosave file behind that cannot be parsed.
  {
    std::string json, rerr;
    if (!readProjectDocument(cur, json, &rerr))
      QFile::remove(cur);
  }
}

void MainWindow::checkAutosaveRecovery() {
  const auto cur = autosavePath();
  const auto prev = autosavePrevPath();
  const bool any =
      QFile::exists(cur) || QFile::exists(prev);

  misuc::project::Session recovered;
  QString best;
  const auto tryFile = [&](const QString& p) {
    std::string json, err;
    if (!readProjectDocument(p, json, &err))
      return false;
    try {
      recovered = misuc::project::Session::fromJson(json);
      best = p;
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };

  if (!(tryFile(cur) || tryFile(prev))) {
    // Nothing usable, but something was left behind: drop the stale/corrupt
    // copies so they cannot shadow a future valid autosave.
    if (any)
      clearAutosave();
    return;
  }
  if (recovered.tracks().empty() && recovered.name().empty())
    return; // nothing meaningful was autosaved
  QMessageBox box(this);
  box.setWindowTitle(QStringLiteral("Recover session"));
  box.setText(QStringLiteral(
      "An unsaved session from the previous run was found.\n"
      "Restore it now?"));
  box.setIcon(QMessageBox::Question);
  QAbstractButton* restoreBtn =
      box.addButton(QStringLiteral("Restore"), QMessageBox::AcceptRole);
  QAbstractButton* discardBtn =
      box.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
  box.exec();
  if (box.clickedButton() == discardBtn) {
    clearAutosave(); // explicit discard: stop nagging on the next launch
    return;
  }
  if (box.clickedButton() != restoreBtn)
    return; // dismissed another way — keep the copy for next time
  restoreSessionFrom(best, true);
}

bool MainWindow::restoreSessionFrom(const QString& path, bool markDirty) {
  std::string json, err;
  if (!readProjectDocument(path, json, &err))
    return false;
  try {
    session_ = misuc::project::Session::fromJson(json);
  } catch (const std::exception&) {
    return false;
  }
  // The copy stores absolute staged asset paths; resolve against its own
  // folder so stored relative paths would still load after a manual move.
  session_.resolveAssets(QFileInfo(path).absolutePath().toStdString());
  projectPath_.clear(); // it is not a document the user chose yet
  dirty_ = markDirty;
  undo_.clear();
  undo_.push(session_);
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  applyTempoToEngine();
  syncLoopState();
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  applyFxToEngine();
  undoCmd_->setEnabled(false);
  redoCmd_->setEnabled(false);
  updateWindowTitle();
  return true;
}

void MainWindow::performUndo() {
  if (!undo_.undo())
    return;
  session_ = undo_.state();
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  channelRack_->refresh();
  applyTempoToEngine();
  syncLoopState();
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  applyFxToEngine();
  dirty_ = true;
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  updateWindowTitle();
}

void MainWindow::performRedo() {
  if (!undo_.redo())
    return;
  session_ = undo_.state();
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  channelRack_->refresh();
  applyTempoToEngine();
  syncLoopState();
  applyContentEndToEngine();
  applyMetronomeToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  applyFxToEngine();
  dirty_ = true;
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  updateWindowTitle();
}

void MainWindow::applyTempoToEngine() { engine_.setTempo(session_.tempo()); }

void MainWindow::applyFxToEngine() {
  std::vector<std::vector<misuc::engine::FxInsertParam>> chains;
  for (const auto& t : session_.tracks()) {
    std::vector<misuc::engine::FxInsertParam> tch;
    for (const auto& fx : t.fx) {
      misuc::engine::FxInsertParam ins;
      ins.uri = fx.uri;
      ins.enabled = fx.enabled;
      for (const auto& p : fx.params)
        ins.params.push_back({p.index, p.value});
      tch.push_back(std::move(ins));
    }
    chains.push_back(std::move(tch));
  }
  // Bus post-fader chains (Comp19): each bus's inserts, with the sidechain key
  // resolved to an engine bus index (-1 = key itself / no key).
  std::vector<std::vector<misuc::engine::FxInsertParam>> busChains;
  for (const auto& b : session_.buses()) {
    std::vector<misuc::engine::FxInsertParam> bch;
    for (const auto& fx : b.fx) {
      misuc::engine::FxInsertParam ins;
      ins.uri = fx.uri;
      ins.enabled = fx.enabled;
      for (const auto& p : fx.params)
        ins.params.push_back({p.index, p.value});
      ins.sidechainBus = -1;
      for (size_t bi = 0; bi < session_.buses().size(); ++bi)
        if (session_.buses()[bi].id == fx.sidechainSource) {
          ins.sidechainBus = static_cast<int>(bi);
          break;
        }
      bch.push_back(std::move(ins));
    }
    busChains.push_back(std::move(bch));
  }
  if (chains == lastFxChains_ && busChains == lastBusFxChains_)
    return; // FX untouched: keep the live instances running as-is.
  lastFxChains_ = chains;
  lastBusFxChains_ = busChains;
  engine_.publishFx(std::move(chains), std::move(busChains));
}

void MainWindow::applyContentEndToEngine() {
  engine_.setContentEnd(session_.contentEndBeats());
}

void MainWindow::applyLoopToEngine(misuc::project::Session& s) {
  engine_.setLoop(s.loopMode(), s.loopStart(), s.loopEnd());
}

void MainWindow::applyMetronomeToEngine() {
  transport_->setCountInOn(session_.metronomeOn());
  transport_->setMetronomeVolume(session_.metronomeVolume());
  transport_->setCountInBeats(session_.countInBeats());
  engine_.setMetronome(session_.metronomeOn(), session_.countInBeats(),
                       session_.metronomeVolume());
}

void MainWindow::onLoopToggled(bool on) {
  if (on) {
    // Turning the loop on always produces a playable span: if the document has
    // no persisted loop yet, loop the whole arrangement (start 0 .. content
    // end). The change lands in the Session so it saves and undoes like every
    // other edit.
    if (!session_.loopMode())
      session_.setLoop(true, 0.0,
                       std::max(1.0, session_.contentEndBeats()));
    onSessionEdited(); // dirty + undo snapshot + engine sync
  } else {
    if (session_.loopMode()) {
      session_.setLoop(false, 0.0, 0.0);
      onSessionEdited();
    }
  }
  syncLoopState();
}

void MainWindow::syncLoopState() {
  // One source of truth (the Session); mirror it everywhere it is shown or
  // heard: engine transport, transport-bar button and piano-roll ruler.
  transport_->setLoopOn(session_.loopMode());
  applyLoopToEngine(session_);
  editorPage_->piano()->setLoop(session_.loopMode(), session_.loopStart(),
                                session_.loopEnd());
}

void MainWindow::updateWindowTitle() {
  const QString& name = QString::fromStdString(session_.name());
  QString title = name;
  if (dirty_)
    title += QStringLiteral(" *");
  title += QStringLiteral(" - Misuc");
  setWindowTitle(title);
  if (headerBar_)
    headerBar_->setProjectName(name.isEmpty() ? QStringLiteral("Untitled") : name);
}

bool MainWindow::confirmDiscard() {
  if (!dirty_)
    return true;
  const auto choice = QMessageBox::question(
      this, QStringLiteral("Unsaved changes"),
      QStringLiteral("Save changes to the current project?"),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (choice == QMessageBox::Cancel)
    return false;
  if (choice == QMessageBox::Save)
    return saveProject();
  return true;
}

} // namespace misuc::ui