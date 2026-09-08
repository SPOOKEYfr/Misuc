#include "MainWindow.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/ClipScore.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/WavFile.h>

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

namespace misuc::ui {

namespace {
const QLatin1String kSettingsOrg("misuc");
const QLatin1String kSettingsApp("misuc");

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
} // namespace

MainWindow::MainWindow(misuc::engine::AudioEngine& engine) : engine_(engine) {
  resize(1280, 800);
  setMinimumSize(1000, 640);
  updateWindowTitle(); // clean slate: "Untitled - Misuc" (no phantom dirty star)

  buildMenus();

  // Global header: brand, transport, live status cluster.
  headerBar_ = new HeaderBar(engine_, this);
  addToolBar(Qt::TopToolBarArea, headerBar_);
  headerBar_->setMovable(false);
  transport_ = headerBar_->transportBar();

  // Transport wiring (loop, record, metronome/count-in).
  connect(transport_, &TransportBar::loopToggled, this, [this](bool on) {
    if (on) {
      applyLoopToEngine(session_);
      editorPage_->piano()->setLoop(true, session_.loopStart(),
                                    session_.loopEnd());
    } else {
      engine_.setLoop(false, 0.0, 0.0);
      editorPage_->piano()->setLoop(false, 0.0, 0.0);
    }
  });
  connect(transport_, &TransportBar::recordToggled, this,
          &MainWindow::onRecordToggled);
  // The click/count-in button is now a real metronome: it arms the engine's
  // metronome and count-in lead-in (previously dead UI). Default off so
  // playback/export stay click-free until the user enables it.
  connect(transport_, &TransportBar::countInToggled, this,
          [this](bool on) { engine_.setMetronome(on, 2.f); });
  engine_.setMetronome(false, 2.f);

  // Central workspace: tabbed real pages.
  workspace_ = new QTabWidget(this);
  workspace_->setObjectName(QStringLiteral("workspaceTabs"));
  workspace_->setDocumentMode(true);
  workspace_->setTabPosition(QTabWidget::North);
  setCentralWidget(workspace_);
  wsMan_ = new WorkspaceManager(this, workspace_);

  // Status strip: contextual hint bar (left) + audio status (right). Created
  // before buildPages() so page wiring can register hints.
  statusBar()->setSizeGripEnabled(false);
  auto* hintBar = new HintBar(statusBar());
  hints_ = new HintManager(hintBar, this);
  statusBar()->addWidget(hintBar, 1);
  status_ = new StatusPanel(engine_, this);
  statusBar()->addPermanentWidget(status_);

  buildPages();

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
}

MainWindow::~MainWindow() = default;

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

  // Patterns (channel-rack-centric for now)
  QMenu* patterns = menuBar()->addMenu(QStringLiteral("&Patterns"));
  patterns->addAction(QStringLiteral("Channel &Rack\tF6"), this,
                      [this] { wsMan_->showWorkspace(workspaces::Id::ChannelRack, true); });
  patterns->addAction(QStringLiteral("&Piano Roll\tF7"), this,
                      [this] { wsMan_->showWorkspace(workspaces::Id::PianoRoll, true); });

  // View: layout presets + workspace toggles
  QMenu* view = menuBar()->addMenu(QStringLiteral("&View"));
  QActionGroup* presetGroup = new QActionGroup(this);
  presetGroup->setExclusive(true);
  const QStringList presets = wsMan_->presetNames();
  for (const QString& name : presets) {
    QAction* act = view->addAction(name, this,
                                   [this, name] { applyLayoutPreset(name); });
    act->setCheckable(true);
    presetGroup->addAction(act);
    if (name == QStringLiteral("Default"))
      act->setChecked(true);
  }
  view->addSeparator();
  auto* showAct = view->addAction(QStringLiteral("&Playlist\tF5"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Playlist, true); });
  auto* showPr  = view->addAction(QStringLiteral("&Piano Roll\tF7"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::PianoRoll, true); });
  auto* showCr  = view->addAction(QStringLiteral("&Channel Rack\tF6"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::ChannelRack, true); });
  auto* showBr  = view->addAction(QStringLiteral("&Browser\tF8"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Browser, true); });
  auto* showMx  = view->addAction(QStringLiteral("&Mixer\tF9"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Mixer, true); });
  auto* showIn  = view->addAction(QStringLiteral("&Inspector\tCtrl+Shift+I"), this,
      [this] { wsMan_->showWorkspace(workspaces::Id::Inspector, true); });
  Q_UNUSED(showAct); Q_UNUSED(showPr); Q_UNUSED(showCr);
  Q_UNUSED(showBr); Q_UNUSED(showMx); Q_UNUSED(showIn);

  // Options
  QMenu* options = menuBar()->addMenu(QStringLiteral("&Options"));
  metronomeCmd_ = options->addAction(QStringLiteral("&Metronome"), this,
      [this](bool on) { engine_.setMetronome(on, 2.f); transport_->setCountInOn(on); },
      QKeySequence(Qt::Key_M));
  metronomeCmd_->setCheckable(true);
  auto* loopOpt = options->addAction(QStringLiteral("&Loop Toggle"), this,
      [this](bool on) { transport_->setLoopOn(on); }, QKeySequence(Qt::Key_L));
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
  themePage_ = new ThemePage(this);
  mixerPage_ = new MixerPage(session_, engine_, this);
  playlist_ = new Playlist(session_, this);
  channelRack_ = new ChannelRack(this);
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
  connect(inspector_, &Inspector::editRequest, this,
          &MainWindow::onInspectorEdit);
  connect(channelRack_, &ChannelRack::edited, this, [this] {
    // The rack model persists via QSettings (not the session); when pattern
    // playback is armed, republish the steps so edits audition live.
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
            const double freq = channelRack_->previewFreq(row);
            float vel = 0.7f;
            const auto& ch = channelRack_->channels();
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
    const std::string rid = session_.addRegion(
        trackId, "Pattern " + std::to_string(bank + 1), start, 4.0);
    auto* region = session_.findRegion(trackId, rid);
    if (!region)
      return;
    const auto& chs = channelRack_->channels();
    for (int row = 0; row < (int)chs.size(); ++row) {
      if (chs[row].muted)
        continue;
      const double freq = channelRack_->previewFreq(row);
      const int pitch = std::clamp(
          static_cast<int>(std::lround(69.0 + 12.0 * std::log2(
                            freq / 440.0))), 0, 127);
      for (int step = 0; step < 16; ++step) {
        if (!chs[row].steps[bank][step])
          continue;
        misuc::project::NoteEvent n;
        n.pitch = static_cast<uint8_t>(pitch);
        n.velocity = 100;
        n.startBeats = step * 0.25;
        n.durationBeats = 0.24;
        region->notes.push_back(n);
      }
    }
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
    engine_.previewSample(std::move(b.samples), std::max<uint32_t>(1, b.channels));
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

  applyContentEndToEngine();
  mixerPage_->syncToEngine();
}

void MainWindow::onSessionEdited() {
  undo_.push(session_);
  dirty_ = true;
  applyTempoToEngine();
  applyLoopToEngine(session_);
  applyContentEndToEngine();
  rebuildMidiScore();
  rebuildClipScore();
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
    applyLoopToEngine(session_);
  }
}

void MainWindow::rebuildRackScore() {
  std::vector<misuc::dsp::MidiNote> notes;
  const auto& ch = channelRack_->channels();
  const int bank = channelRack_->pattern();
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
    for (int step = 0; step < 16; ++step) {
      if (step >= c.steps[bank].size() || !c.steps[bank][step])
        continue;
      misuc::dsp::MidiNote mn;
      double start = step * 0.25;
      if ((step & 1) && swingShift > 0.0)
        start += swingShift;
      mn.startBeats = start;
      mn.durationBeats = 0.20;
      mn.freq = channelRack_->previewFreq(row);
      mn.gain = c.volume;
      mn.track = static_cast<uint32_t>(row);
      notes.push_back(mn);
    }
  }
  std::sort(notes.begin(), notes.end(),
            [](const misuc::dsp::MidiNote& a, const misuc::dsp::MidiNote& b) {
              return a.startBeats < b.startBeats;
            });
  engine_.setScore(std::move(notes));
}

void MainWindow::onRecordToggled(bool on) {
  if (on) {
    // Arm recording. Start from the current transport position so the clip
    // lands exactly where it was recorded.
    const double bpm = session_.tempo();
    if (bpm > 0.0) {
      recordStartBeats_ =
          static_cast<double>(engine_.playheadSamples()) /
          ((60.0 / bpm) * engine_.sampleRate());
    } else {
      recordStartBeats_ = 0.0;
    }
    if (!engine_.isTransportPlaying())
      engine_.play(false);
    engine_.startRecord();
    return;
  }

  engine_.stopRecord();
  std::vector<float> samples;
  uint32_t sr = 0, ch = 0;
  if (!engine_.takeRecordedSamples(samples, sr, ch) || sr == 0 ||
      ch == 0 || samples.empty())
    return;

  // Stage the recording next to the project (like an import) and place the
  // clip on the currently selected track at the recording's start beat.
  ++recordCounter_;
  const QString baseName = QStringLiteral("clip_%1").arg(recordCounter_, 3, 10,
                                                         QLatin1Char('0'));
  const QString wavName = baseName + QStringLiteral("_%1.wav")
                              .arg(QDateTime::currentMSecsSinceEpoch());
  const QString projectHome =
      projectPath_.isEmpty()
          ? QDir::tempPath()
          : QFileInfo(projectPath_).absolutePath();
  QDir().mkpath(QDir(projectHome).filePath(QStringLiteral("audio")));
  const QString wavPath =
      QDir(projectHome).filePath(QStringLiteral("audio/") + wavName);
  std::string werr;
  if (!misuc::audio::writeWavSamples(wavPath.toStdString(), samples.data(),
                                     samples.size(), 1, sr,
                                     /*pcm16=*/false, werr))
    return;

  misuc::project::Track* t =
      session_.findTrack(editorPage_->piano()->trackId().toStdString());
  std::string tid;
  if (t) {
    tid = t->id;
  } else {
    if (session_.tracks().empty())
      session_.addTrack("Track 1");
    tid = session_.tracks().front().id;
    editorPage_->piano()->setTrack(QString::fromStdString(tid));
  }

  misuc::project::AudioClip c;
  c.name = baseName.toStdString();
  c.sourceFile = wavPath.toStdString();
  c.startBeats = recordStartBeats_;
  c.durationBeats = session_.tempo() > 0.0
                        ? (samples.size() / static_cast<double>(sr)) *
                              session_.tempo() / 60.0
                        : 1.0;
  c.sampleRate = sr;
  c.channels = 1;
  session_.addClip(tid, std::move(c));
  onSessionEdited();
}

void MainWindow::exportAudio() {
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

  QString path = exportPathOverride_;
  if (path.isEmpty()) {
    const QString home = projectPath_.isEmpty()
                             ? QDir::homePath()
                             : QFileInfo(projectPath_).absolutePath();
    const QString base = projectPath_.isEmpty()
                             ? QStringLiteral("untitled")
                             : QFileInfo(projectPath_).completeBaseName();
    path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export audio"),
        QDir(home).filePath(base + QStringLiteral("-export.wav")),
        QStringLiteral("WAV audio (*.wav)"));
    if (path.isEmpty()) {
      restore();
      return;
    }
    if (!path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive))
      path += QStringLiteral(".wav");
  }

  // Bounce the extent that contains all arranged material (notes, clips, loop
  // region), with a minimum of one bar.
  double endBeats = 4.0;
  for (const auto& tr : session_.tracks()) {
    for (const auto& n : tr.notes)
      endBeats = std::max(endBeats, n.startBeats + n.durationBeats);
    for (const auto& c : tr.clips)
      endBeats = std::max(endBeats, c.startBeats + c.durationBeats);
  }
  if (session_.loopMode())
    endBeats = std::max(endBeats, session_.loopEnd());

  std::string err;
  if (!engine_.renderToWav(path.toStdString(), 0.0, endBeats, 2, err)) {
    restore();
    QMessageBox::warning(this, QStringLiteral("Export failed"),
                         QString::fromStdString(err));
    return;
  }
  restore();
  statusBar()->showMessage(
      QStringLiteral("Exported %1 (%2 beats)").arg(path).arg(endBeats, 0, 'f', 1));
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
  inspector_->showTrack(sel.trackRow, sel.trackName, sel.trackMuted,
                        sel.trackSolo, sel.trackGainDb, sel.trackPan);
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
        const bool on = !engine_.isRecording();
        transport_->setRecording(on);
        onRecordToggled(on);
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
  if (workspace_)
    persistLayout();
  event->accept();
}

void MainWindow::newProject() {
  if (!confirmDiscard())
    return;
  session_ = misuc::project::Session();
  projectPath_.clear();
  dirty_ = false;
  undo_.clear();
  undo_.push(session_);
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  applyTempoToEngine();
  applyContentEndToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  undoCmd_->setEnabled(false);
  redoCmd_->setEnabled(false);
  updateWindowTitle();
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
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, QStringLiteral("Open failed"), f.errorString());
    return;
  }
  try {
    session_ =
        misuc::project::Session::fromJson(f.readAll().toStdString());
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
  projectPage_->refresh();
  editorPage_->refresh();
  mixerPage_->refresh();
  mixerPage_->syncToEngine();
  applyTempoToEngine();
  applyLoopToEngine(session_);
  applyContentEndToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  undoCmd_->setEnabled(false);
  redoCmd_->setEnabled(false);
  updateWindowTitle();
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
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(this, QStringLiteral("Save failed"), f.errorString());
    return false;
  }
  // Serialize a relativized copy so the document is portable alongside its
  // `<folder>/audio/` assets; the live session keeps absolute paths.
  misuc::project::Session copy = session_;
  copy.relativizeAssets(QFileInfo(path).absolutePath().toStdString());
  f.write(QByteArray::fromStdString(copy.toJson()));
  projectPath_ = path;
  dirty_ = false;
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
  applyTempoToEngine();
  applyLoopToEngine(session_);
  applyContentEndToEngine();
  rebuildMidiScore();
  rebuildClipScore();
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
  applyTempoToEngine();
  applyLoopToEngine(session_);
  applyContentEndToEngine();
  rebuildMidiScore();
  rebuildClipScore();
  dirty_ = true;
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  updateWindowTitle();
}

void MainWindow::applyTempoToEngine() { engine_.setTempo(session_.tempo()); }

void MainWindow::applyContentEndToEngine() {
  engine_.setContentEnd(session_.contentEndBeats());
}

void MainWindow::applyLoopToEngine(misuc::project::Session& s) {
  engine_.setLoop(s.loopMode(), s.loopStart(), s.loopEnd());
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