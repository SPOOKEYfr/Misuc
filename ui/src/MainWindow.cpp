#include "MainWindow.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <QApplication>
#include <QActionGroup>
#include <QCloseEvent>
#include <QDateTime>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
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

namespace misuc::ui {

namespace {
const QLatin1String kSettingsOrg("misuc");
const QLatin1String kSettingsApp("misuc");
} // namespace

MainWindow::MainWindow(misuc::engine::AudioEngine& engine) : engine_(engine) {
  resize(1280, 800);
  setMinimumSize(1000, 640);
  setWindowTitle(QStringLiteral("Untitled * - Misuc"));

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

  // Keyboard shortcuts and layout presets for the modular shell.
  auto* pianoTab = new QAction(QStringLiteral("Piano Roll"), this);
  pianoTab->setShortcuts({QKeySequence(Qt::Key_F7)});
  connect(pianoTab, &QAction::triggered, this, [this] {
    workspace_->setCurrentWidget(editorPage_);
    editorPage_->piano()->setFocus(Qt::ShortcutFocusReason);
  });
  addAction(pianoTab);

  auto* mixerToggle = new QAction(QStringLiteral("mixerToggle"), this);
  mixerToggle->setShortcuts(
      {QKeySequence(Qt::Key_F9), QKeySequence(QStringLiteral("Ctrl+Shift+M"))});
  connect(mixerToggle, &QAction::triggered, this, [this] {
    mixerDock_->setVisible(!mixerDock_->isVisible());
  });
  addAction(mixerToggle);

  auto* spaceAct = new QAction(QStringLiteral("playStop"), this);
  spaceAct->setShortcut(QKeySequence(Qt::Key_Space));
  connect(spaceAct, &QAction::triggered, this, &MainWindow::togglePlayStop);
  addAction(spaceAct);

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
  QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
  QAction* newAct =
      file->addAction(QStringLiteral("&New"), this, &MainWindow::newProject,
                      QKeySequence::New);
  QAction* openAct =
      file->addAction(QStringLiteral("&Open..."), this, &MainWindow::openDialog,
                      QKeySequence::Open);
  file->addSeparator();
  QAction* saveAct = file->addAction(
      QStringLiteral("&Save"), this, &MainWindow::saveProject,
      QKeySequence::Save);
  QAction* saveAsAct = file->addAction(
      QStringLiteral("Save &As..."), this, &MainWindow::saveProjectAs,
      QKeySequence::SaveAs);
  file->addSeparator();
  QAction* exportAct = file->addAction(
      QStringLiteral("Export &Audio..."), this, &MainWindow::exportAudio,
      QKeySequence(Qt::CTRL | Qt::Key_E));
  file->addSeparator();
  file->addAction(QStringLiteral("&Quit"), this, &QWidget::close,
                  QKeySequence::Quit);
  Q_UNUSED(newAct);
  Q_UNUSED(openAct);
  Q_UNUSED(saveAct);
  Q_UNUSED(saveAsAct);
  Q_UNUSED(exportAct);

  QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
  undoCmd_ = edit->addAction(QStringLiteral("&Undo"), this,
                             &MainWindow::performUndo,
                             QKeySequence::Undo);
  redoCmd_ = edit->addAction(QStringLiteral("&Redo"), this,
                             &MainWindow::performRedo,
                             QKeySequence::Redo);
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());

  // Workspace: layout presets + panel toggles for the modular shell.
  QMenu* workspace = menuBar()->addMenu(QStringLiteral("&Workspace"));
  QActionGroup* presetGroup = new QActionGroup(this);
  presetGroup->setExclusive(true);
  const QStringList presets = {QStringLiteral("Default"),
                               QStringLiteral("Mixing"),
                               QStringLiteral("Piano Roll"),
                               QStringLiteral("Recording")};
  for (const QString& name : presets) {
    QAction* act =
        workspace->addAction(name, this, [this, name] { applyLayoutPreset(name); });
    act->setCheckable(true);
    presetGroup->addAction(act);
    if (name == QStringLiteral("Default"))
      act->setChecked(true);
  }
  workspace->addSeparator();
  workspace->addAction(QStringLiteral("Mixer\tF9"), this, [this] {
    mixerDock_->setVisible(!mixerDock_->isVisible());
  });

  QMenu* help = menuBar()->addMenu(QStringLiteral("&Help"));
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

  workspace_->addTab(projectPage_, QStringLiteral("Project"));
  workspace_->addTab(editorPage_, QStringLiteral("Editor"));
  workspace_->addTab(audioSettings_, QStringLiteral("Audio"));
  workspace_->addTab(themePage_, QStringLiteral("Appearance"));

  // The mixer is a dockable panel, not a tab.
  mixerDock_ = new QDockWidget(QStringLiteral("Mixer"), this);
  mixerDock_->setObjectName(QStringLiteral("mixerDock"));
  mixerDock_->setWidget(mixerPage_);
  mixerDock_->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea |
                              Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
  mixerDock_->setFeatures(QDockWidget::DockWidgetClosable |
                          QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable);
  addDockWidget(Qt::BottomDockWidgetArea, mixerDock_);

  editorPage_->importAudioSink = [this](const QString& src) {
    return stageAudioFile(src);
  };

  connect(projectPage_, &ProjectPage::edited, this,
          &MainWindow::onSessionEdited);
  connect(editorPage_, &EditorPage::edited, this,
          &MainWindow::onSessionEdited);
  connect(mixerPage_, &MixerPage::edited, this,
          &MainWindow::onSessionEdited);

  // Keep the track selection mirrored between the editor list and the mixer
  // strips; each side only echoes back a state change, never re-broadcasts it.
  connect(editorPage_, &EditorPage::trackSelected, this,
          [this](const QString& id) { mixerPage_->selectTrack(id); });
  connect(mixerPage_, &MixerPage::trackSelected, this,
          [this](const QString& id) { editorPage_->selectTrack(id, false); });

  // Note audition: preview the pitch immediately, even while stopped.
  connect(editorPage_->piano(), &PianoRoll::audition, this,
          [this](double freq, float vel) { engine_.previewNote(freq, vel); });

  // Wire contextual hints over the chrome and workspace controls.
  const auto& th = ThemeEngine::instance();
  Q_UNUSED(th);
  hints_->registerHint(projectPage_, QStringLiteral("Project"),
                       QStringLiteral("Project name, tempo and time signature"),
                       QStringLiteral("Ctrl+N/O/S"));
  hints_->registerHint(editorPage_, QStringLiteral("Piano Roll"),
                       QStringLiteral("Draw and edit MIDI notes"),
                       QStringLiteral("F7"));
  hints_->registerHint(mixerPage_, QStringLiteral("Mixer"),
                       QStringLiteral("Per-track gain/pan/mute/solo and meters"),
                       QStringLiteral("F9"));
  hints_->registerHint(audioSettings_, QStringLiteral("Audio Settings"),
                       QStringLiteral("Device, sample rate, buffer and monitoring"),
                       QString(), nullptr);
  hints_->registerHint(themePage_, QStringLiteral("Appearance"),
                       QStringLiteral("Theme presets, accent and glass"),
                       QString(), nullptr);

  mixerPage_->syncToEngine();
}

void MainWindow::onSessionEdited() {
  undo_.push(session_);
  dirty_ = true;
  applyTempoToEngine();
  applyLoopToEngine(session_);
  rebuildMidiScore();
  rebuildClipScore();
  mixerPage_->syncFromSession();
  mixerPage_->syncToEngine();
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  updateWindowTitle();
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
  // Real layout changes: dock visibility + active editor tab per workflow.
  if (name == QStringLiteral("Piano Roll")) {
    mixerDock_->setVisible(false);
    workspace_->setCurrentWidget(editorPage_);
  } else if (name == QStringLiteral("Recording")) {
    mixerDock_->setVisible(true);
    workspace_->setCurrentWidget(editorPage_);
  } else { // Default / Mixing
    mixerDock_->setVisible(true);
    workspace_->setCurrentWidget(projectPage_);
  }
}

void MainWindow::persistLayout() {
  QSettings settings(kSettingsOrg, kSettingsApp);
  settings.setValue(QStringLiteral("workspace/state"), saveState());
  settings.setValue(QStringLiteral("workspace/tab"),
                    workspace_->currentIndex());
  settings.setValue(QStringLiteral("workspace/size"), size());
}

void MainWindow::restoreLayout() {
  QSettings settings(kSettingsOrg, kSettingsApp);
  const QVariant state = settings.value(QStringLiteral("workspace/state"));
  if (state.isValid())
    restoreState(state.toByteArray());
  const QVariant tab = settings.value(QStringLiteral("workspace/tab"));
  if (tab.isValid() && tab.toInt() >= 0 && tab.toInt() < workspace_->count())
    workspace_->setCurrentIndex(tab.toInt());
  const QVariant size = settings.value(QStringLiteral("workspace/size"));
  const QSize saved = size.toSize();
  if (saved.isValid() && !saved.isEmpty())
    resize(saved);
}

void MainWindow::showAbout() {
  misuc::ui::AboutDialog dlg(this);
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
  rebuildMidiScore();
  rebuildClipScore();
  dirty_ = true;
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
  updateWindowTitle();
}

void MainWindow::applyTempoToEngine() { engine_.setTempo(session_.tempo()); }

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