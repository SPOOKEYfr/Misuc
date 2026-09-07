#include "EditorPage.h"

#include <cmath>

#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>

#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/project/Session.h>

#include "PianoRoll.h"
#include "Theme.h"

using misuc::project::Session;
using misuc::project::Track;
using misuc::project::AudioClip;

namespace misuc::ui {

EditorPage::EditorPage(Session& session, QWidget* parent)
    : QWidget(parent), session_(&session) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 24);
  root->setSpacing(16);

  {
    auto* t = new QLabel(QStringLiteral("Editor"), this);
    t->setFont(theme::uiFont(13, QFont::Bold));
    root->addWidget(t);
    auto* s = new QLabel(
        QStringLiteral("Add tracks and draw MIDI notes. Snap and zoom are "
                       "applied live; every edit is undoable."),
        this);
    s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    s->setWordWrap(true);
    root->addWidget(s);
  }

  auto* body = new QHBoxLayout();
  body->setSpacing(16);

  // --- Track column ------------------------------------------------------
  auto* trackCol = new QVBoxLayout();
  trackCol->setSpacing(8);

  auto* trackHead = new QLabel(QStringLiteral("Tracks"), this);
  trackHead->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  trackCol->addWidget(trackHead);

  auto* addBtn = new QPushButton(QStringLiteral("+ Add Track"), this);
  addBtn->setObjectName(QStringLiteral("smallButton"));
  connect(addBtn, &QPushButton::clicked, this, [this] { addTrack(); });
  trackCol->addWidget(addBtn);

  trackList_ = new QListWidget(this);
  trackList_->setSelectionMode(QAbstractItemView::SingleSelection);
  trackList_->setMinimumWidth(220);
  trackList_->setStyleSheet(QStringLiteral(
      "QListWidget { background-color: %1; border: 1px solid %2; "
      "border-radius: 8px; padding: 4px; }")
                                .arg(theme::kPanelSunken.name(), theme::kBorder.name()));
  connect(trackList_, &QListWidget::currentRowChanged, this,
          &EditorPage::onSelectionChanged);
  trackCol->addWidget(trackList_, 1);

  auto* removeBtn = new QPushButton(QStringLiteral("Remove Selected"), this);
  removeBtn->setObjectName(QStringLiteral("smallButton"));
  connect(removeBtn, &QPushButton::clicked, this, [this] { removeTrack(); });
  trackCol->addWidget(removeBtn);

  body->addLayout(trackCol, 0);

  // --- Piano roll column --------------------------------------------------
  auto* editorCol = new QVBoxLayout();
  editorCol->setSpacing(8);

  auto* toolbar = new QHBoxLayout();
  toolbar->setSpacing(12);
  auto* zoomLabel = new QLabel(QStringLiteral("Zoom"), this);
  zoomLabel->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  zoomSlider_ = new QSlider(Qt::Horizontal, this);
  zoomSlider_->setRange(10, 160);
  zoomSlider_->setValue(40);
  zoomSlider_->setFixedWidth(180);
  snapChk_ = new QCheckBox(QStringLiteral("Snap 1/16"), this);
  snapChk_->setChecked(true);
  connect(zoomSlider_, &QSlider::valueChanged, this, [this](int v) {
    piano_->setZoom(static_cast<double>(v));
  });
  connect(snapChk_, &QCheckBox::toggled, this,
          [this](bool on) { piano_->setSnap(on); });
  auto* quantizeBtn = new QPushButton(QStringLiteral("Quantize"), this);
  connect(quantizeBtn, &QPushButton::clicked, this, [this] {
    piano_->quantize();
  });
  auto* importBtn = new QPushButton(QStringLiteral("Import Audio\u2026"), this);
  connect(importBtn, &QPushButton::clicked, this, &EditorPage::importAudio);
  toolbar->addWidget(zoomLabel);
  toolbar->addWidget(zoomSlider_);
  toolbar->addWidget(snapChk_);
  toolbar->addWidget(quantizeBtn);
  toolbar->addWidget(importBtn);
  toolbar->addStretch();
  editorCol->addLayout(toolbar);

  piano_ = new PianoRoll(*session_, this);
  connect(piano_, &PianoRoll::edited, this,
          &EditorPage::emitEdited);
  editorCol->addWidget(piano_, 1);

  body->addLayout(editorCol, 1);

  root->addLayout(body, 1);

  if (session_->tracks().empty())
    session_->addTrack("Track 1");

  rebuildTrackList();
  if (!session_->tracks().empty())
    onSelectionChanged(trackList_->currentRow());
}

void EditorPage::refresh() {
  rebuildTrackList();
  piano_->refresh();
}

void EditorPage::rebuildTrackList() {
  const QString keep = selectedId_;
  trackList_->clear();
  int row = 0;
  for (const Track& t : session_->tracks()) {
    auto* item = new QListWidgetItem();
    item->setData(Qt::UserRole, QString::fromStdString(t.id));
    trackList_->addItem(item);

    auto* rowW = new QWidget(trackList_);
    auto* lay = new QHBoxLayout(rowW);
    lay->setContentsMargins(10, 6, 10, 6);
    lay->setSpacing(10);
    auto* name = new QLabel(QString::fromStdString(t.name), rowW);
    name->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;")
            .arg(theme::kText.name()));
    auto* mute = new QCheckBox(QStringLiteral("Mute"), rowW);
    mute->setChecked(t.muted);
    connect(mute, &QCheckBox::toggled, this, [this, tId = t.id](bool on) {
      Track* tr = session_->findTrack(tId);
      if (tr) {
        tr->muted = on;
        emitEdited();
      }
    });
    lay->addWidget(name);
    lay->addStretch();
    lay->addWidget(mute);
    trackList_->setItemWidget(item, rowW);

    if (t.id == keep.toStdString())
      trackList_->setCurrentRow(row);
    ++row;
  }
  if (keep.isEmpty() && !session_->tracks().empty())
    trackList_->setCurrentRow(0);
}

void EditorPage::addTrack() {
  const int n = static_cast<int>(session_->tracks().size()) + 1;
  const std::string id = session_->addTrack("Track " + std::to_string(n));
  emitEdited();
  rebuildTrackList();
  for (int r = 0; r < trackList_->count(); ++r) {
    if (trackList_->item(r)->data(Qt::UserRole).toString()
            .toStdString() == id) {
      trackList_->setCurrentRow(r);
      break;
    }
  }
}

void EditorPage::removeTrack() {
  auto* item = trackList_->currentItem();
  if (!item)
    return;
  const std::string id = item->data(Qt::UserRole).toString().toStdString();
  if (!session_->removeTrack(id))
    return;
  selectedId_.clear();
  piano_->setTrack(QString());
  emitEdited();
  rebuildTrackList();
}

void EditorPage::importAudio() {
  const QString src = QFileDialog::getOpenFileName(
      this, QStringLiteral("Import audio"),
      QString(),
      QStringLiteral("Audio files (*.wav *.flac *.ogg *.aiff *.aif)"));
  if (src.isEmpty())
    return;

  misuc::audio::AudioBuffer buf;
  std::string err;
  if (!misuc::audio::readAudioFile(src.toStdString(), buf, err)) {
    QMessageBox::warning(this, QStringLiteral("Import failed"),
                         QString::fromStdString(err));
    return;
  }

  QString final = src;
  if (importAudioSink)
    final = importAudioSink(src);
  if (final.isEmpty())
    return;

  const std::string want = piano_->trackId().toStdString();
  Track* t = session_->findTrack(want);
  std::string tid = want;
  if (!t) {
    if (session_->tracks().empty())
      session_->addTrack("Track 1");
    tid = session_->tracks().front().id;
  }

  AudioClip c;
  c.name = QFileInfo(final).completeBaseName().toStdString();
  c.sourceFile = final.toStdString();
  c.durationBeats = session_->tempo() > 0.0
                        ? buf.durationSeconds() * session_->tempo() / 60.0
                        : 1.0;
  c.sampleRate =
      static_cast<uint32_t>(std::lround(buf.sampleRate));
  c.channels = static_cast<uint16_t>(buf.channels);
  session_->addClip(tid, std::move(c));

  selectedId_ = QString::fromStdString(tid);
  emitEdited();
  piano_->setTrack(QString::fromStdString(tid));
  for (int r = 0; r < trackList_->count(); ++r) {
    if (trackList_->item(r)->data(Qt::UserRole).toString().toStdString() ==
        tid) {
      trackList_->setCurrentRow(r);
      break;
    }
  }
}

void EditorPage::onSelectionChanged(int row) {
  if (row < 0 || row >= trackList_->count())
    return;
  const QString id = trackList_->item(row)->data(Qt::UserRole).toString();
  selectedId_ = id;
  piano_->setTrack(id);
}

} // namespace misuc::ui