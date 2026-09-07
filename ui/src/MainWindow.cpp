#include "MainWindow.h"

#include <memory>
#include <utility>

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QPainter>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/ClipScore.h>
#include <misuc/engine/audio/MidiScore.h>
#include <misuc/engine/audio/WavFile.h>

#include "AudioSettingsPage.h"
#include "EditorPage.h"
#include "ProjectPage.h"
#include "StatusPanel.h"
#include "Theme.h"
#include "TransportBar.h"
#include "PianoRoll.h"

namespace misuc::ui {

MainWindow::MainWindow(misuc::engine::AudioEngine& engine) : engine_(engine) {
  resize(1180, 760);
  setMinimumSize(960, 620);
  setWindowTitle(QStringLiteral("Untitled * - Misuc"));

  buildMenus();

  auto* central = new QWidget(this);
  central->setObjectName(QStringLiteral("central"));
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // Top transport bar.
  transport_ = new TransportBar(engine_, this);
  root->addWidget(transport_);
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

  // Middle: nav rail + page stack.
  auto* middle = new QWidget(central);
  auto* midLay = new QHBoxLayout(middle);
  midLay->setContentsMargins(0, 0, 0, 0);
  midLay->setSpacing(0);

  navRail_ = new QWidget(middle);
  navRail_->setFixedWidth(150);
  auto* navLay = new QVBoxLayout(navRail_);
  navLay->setContentsMargins(10, 18, 10, 18);
  navLay->setSpacing(4);

  auto* navGroup = new QButtonGroup(this);
  navGroup->setExclusive(true);

  auto addNav = [this, navLay, navGroup](const QString& label, int page) {
    auto* btn = new QPushButton(label, navRail_);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setProperty("navPage", page);
    btn->setObjectName(QStringLiteral("navButton"));
    if (navGroup->buttons().isEmpty())
      btn->setChecked(true);
    navGroup->addButton(btn, page);
    navLay->addWidget(btn);
    connect(btn, &QPushButton::clicked, this, [this, page] {
      pages_->setCurrentIndex(page);
    });
  };

  addNav(QStringLiteral("Project"), 0);
  addNav(QStringLiteral("Editor"), 1);
  addNav(QStringLiteral("Audio"), 2);
  navLay->addStretch();

  midLay->addWidget(navRail_);
  pages_ = new QStackedWidget(middle);
  midLay->addWidget(pages_, 1);
  root->addWidget(middle, 1);

  // Status strip.
  status_ = new StatusPanel(engine_, this);
  root->addWidget(status_);

  setCentralWidget(central);

  buildPages();
  pages_->setCurrentIndex(0);

  // Seed the undo history so the first edit has something to revert to.
  undo_.push(session_);
  rebuildMidiScore();
  rebuildClipScore();

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
  file->addAction(QStringLiteral("&Quit"), this, &QWidget::close,
                  QKeySequence::Quit);
  Q_UNUSED(newAct);
  Q_UNUSED(openAct);
  Q_UNUSED(saveAct);
  Q_UNUSED(saveAsAct);

  QMenu* edit = menuBar()->addMenu(QStringLiteral("&Edit"));
  undoCmd_ = edit->addAction(QStringLiteral("&Undo"), this,
                             &MainWindow::performUndo,
                             QKeySequence::Undo);
  redoCmd_ = edit->addAction(QStringLiteral("&Redo"), this,
                             &MainWindow::performRedo,
                             QKeySequence::Redo);
  undoCmd_->setEnabled(undo_.canUndo());
  redoCmd_->setEnabled(undo_.canRedo());
}

void MainWindow::buildPages() {
  projectPage_ = new ProjectPage(session_, this);
  editorPage_ = new EditorPage(session_, this);
  pages_->addWidget(projectPage_);
  pages_->addWidget(editorPage_);
  pages_->addWidget(new AudioSettingsPage(engine_, this));

  editorPage_->importAudioSink = [this](const QString& src) {
    return stageAudioFile(src);
  };

  connect(projectPage_, &ProjectPage::edited, this,
          &MainWindow::onSessionEdited);
  connect(editorPage_, &EditorPage::edited, this,
          &MainWindow::onSessionEdited);

  // Note audition: preview the pitch immediately, even while stopped.
  connect(editorPage_->piano(), &PianoRoll::audition, this,
          [this](double freq, float vel) { engine_.previewNote(freq, vel); });
}

void MainWindow::onSessionEdited() {
  undo_.push(session_);
  dirty_ = true;
  applyTempoToEngine();
  applyLoopToEngine(session_);
  rebuildMidiScore();
  rebuildClipScore();
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
  transport_->refresh();
  status_->refresh();
  feedPlayhead();
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

void MainWindow::closeEvent(QCloseEvent* event) {
  if (!confirmDiscard()) {
    event->ignore();
    return;
  }
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
  projectPath_ = path;
  dirty_ = false;
  undo_.clear();
  undo_.push(session_);
  projectPage_->refresh();
  editorPage_->refresh();
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
  f.write(QByteArray::fromStdString(session_.toJson()));
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