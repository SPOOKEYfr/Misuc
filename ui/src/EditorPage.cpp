#include "EditorPage.h"

#include <cmath>

#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVariant>
#include <QVBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>

#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/project/Session.h>

#include "PianoRoll.h"
#include "Theme.h"
#include "HintSystem.h"

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
  addBtn->setObjectName(QStringLiteral("addTrackBtn"));
  addBtn->setIcon(icons::make(icons::IconId::Add, theme::kText, 14));
  addBtn->setIconSize(QSize(14, 14));
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
  removeBtn->setObjectName(QStringLiteral("removeTrackBtn"));
  removeBtn->setIcon(icons::make(icons::IconId::Remove, theme::kText, 14));
  removeBtn->setIconSize(QSize(14, 14));
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
  zoomSlider_->setObjectName(QStringLiteral("editorZoom"));
  zoomSlider_->setRange(10, 160);
  zoomSlider_->setValue(40);
  zoomSlider_->setFixedWidth(180);
  snapChk_ = new QCheckBox(QStringLiteral("Snap"), this);
  snapChk_->setObjectName(QStringLiteral("editorSnapChk"));
  snapChk_->setChecked(true);
  snapCombo_ = new QComboBox(this);
  snapCombo_->setObjectName(QStringLiteral("editorSnapCombo"));
  for (const double sb : PianoRoll::snapBeatsOptions())
    snapCombo_->addItem(QStringLiteral("1/%1").arg(static_cast<int>(1.0 / sb)),
                        QVariant::fromValue(sb));
  snapCombo_->setCurrentIndex(2); // 1/16
  snapCombo_->setFocusPolicy(Qt::NoFocus);
  connect(snapChk_, &QCheckBox::toggled, this,
          [this](bool on) { piano_->setSnap(on); });
  connect(snapCombo_, &QComboBox::activated, this, [this](int i) {
    const double sb = snapCombo_->itemData(i).toDouble();
    piano_->setSnapBeats(sb);
    snapChk_->setChecked(true);
  });
  rowHSlider_ = new QSlider(Qt::Horizontal, this);
  rowHSlider_->setObjectName(QStringLiteral("editorRowHeight"));
  rowHSlider_->setRange(12, 48);
  rowHSlider_->setValue(18);
  rowHSlider_->setFixedWidth(140);
  auto* rowHLabel = new QLabel(QStringLiteral("Rows"), this);
  rowHLabel->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  connect(zoomSlider_, &QSlider::valueChanged, this, [this](int v) {
    piano_->setZoom(static_cast<double>(v));
  });
  connect(rowHSlider_, &QSlider::valueChanged, this,
          [this](int v) { piano_->setRowHeight(v); });
  auto* quantizeBtn = new QPushButton(QStringLiteral("Quantize"), this);
  quantizeBtn->setObjectName(QStringLiteral("quantizeBtn"));
  quantizeBtn->setIcon(icons::make(icons::IconId::Snap, theme::kText, 14));
  quantizeBtn->setIconSize(QSize(14, 14));
  connect(quantizeBtn, &QPushButton::clicked, this, [this] {
    piano_->quantize();
  });
  auto* importBtn = new QPushButton(QStringLiteral("Import Audio\u2026"), this);
  importBtn->setObjectName(QStringLiteral("importAudioBtn"));
  importBtn->setIcon(icons::make(icons::IconId::Folder, theme::kText, 14));
  importBtn->setIconSize(QSize(14, 14));
  connect(importBtn, &QPushButton::clicked, this, &EditorPage::importAudio);
  toolbar->addWidget(zoomLabel);
  toolbar->addWidget(zoomSlider_);
  toolbar->addWidget(rowHLabel);
  toolbar->addWidget(rowHSlider_);
  toolbar->addWidget(snapChk_);
  toolbar->addWidget(snapCombo_);
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

    auto* rowW = new QWidget();
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

void EditorPage::selectTrack(const QString& id, bool notify) {
  for (int r = 0; r < trackList_->count(); ++r) {
    if (trackList_->item(r)->data(Qt::UserRole).toString() == id) {
      const bool block = trackList_->signalsBlocked();
      trackList_->blockSignals(!notify);
      trackList_->setCurrentRow(r);
      trackList_->blockSignals(block);
      selectedId_ = id;
      piano_->setTrack(id);
      if (notify)
        emit trackSelected(id);
      return;
    }
  }
}

void EditorPage::onSelectionChanged(int row) {
  if (row < 0 || row >= trackList_->count())
    return;
  const QString id = trackList_->item(row)->data(Qt::UserRole).toString();
  selectedId_ = id;
  piano_->setTrack(id);
  emit trackSelected(id);
}

void EditorPage::attachHints(HintManager* hints) {
  hints_ = hints;
  if (!hints)
    return;
  const QPointer<EditorPage> self(this);
  auto* addBtn = findChild<QPushButton*>(QStringLiteral("addTrackBtn"));
  if (addBtn)
    hints->registerTool(addBtn, QStringLiteral("Add Track"),
                        QStringLiteral("Append a new MIDI track and select it"),
                        QString(), icons::IconId::Add);
  auto* removeBtn = findChild<QPushButton*>(QStringLiteral("removeTrackBtn"));
  if (removeBtn)
    hints->registerTool(removeBtn, QStringLiteral("Remove Selected"),
                        QStringLiteral("Delete the selected track, its clips "
                                       "and its rack channel"), QString(),
                        icons::IconId::Remove);
  if (trackList_)
    hints->registerTool(trackList_, QStringLiteral("Track List"),
                        QStringLiteral("Select the track the piano roll draws "
                                       "and edits"), QString(), icons::IconId::PianoRoll);
  if (zoomSlider_)
    hints->registerTool(zoomSlider_, QStringLiteral("Editor Zoom"),
                        QStringLiteral("Horizontal zoom of the piano roll grid"),
                        QString(), icons::IconId::ZoomIn,
                        [self]() -> QString {
                          if (self.isNull() || !self->zoomSlider_)
                            return QString();
                          return QStringLiteral("%1%").arg(self->zoomSlider_->value());
                        });
  if (rowHSlider_)
    hints->registerTool(rowHSlider_, QStringLiteral("Piano Roll Row Height"),
                        QStringLiteral("Vertical height of the note rows"),
                        QString(), icons::IconId::ZoomIn,
                        [self]() -> QString {
                          if (self.isNull() || !self->rowHSlider_)
                            return QString();
                          return QStringLiteral("%1 px").arg(self->rowHSlider_->value());
                        });
  if (snapChk_)
    hints->registerTool(snapChk_, QStringLiteral("Snap"),
                        QStringLiteral("Constrain note drags and quantize to "
                                       "the selected grid"), QString(),
                        icons::IconId::Snap,
                        [self]() -> QString {
                          if (self.isNull() || !self->snapChk_) return QString();
                          return self->snapChk_->isChecked() ? QStringLiteral("on")
                                                             : QStringLiteral("off");
                        });
  if (snapCombo_)
    hints->registerTool(snapCombo_, QStringLiteral("Grid"),
                        QStringLiteral("Grid resolution for snap and quantize"),
                        QString(), icons::IconId::Snap,
                        [self]() -> QString {
                          if (self.isNull() || !self->snapCombo_) return QString();
                          return self->snapCombo_->currentText();
                        });
  auto* quantize = findChild<QPushButton*>(QStringLiteral("quantizeBtn"));
  if (quantize)
    hints->registerTool(quantize, QStringLiteral("Quantize"),
                        QStringLiteral("Snap the selection's note starts to "
                                       "the active grid"), QString(),
                        icons::IconId::Snap);
  auto* importBtn = findChild<QPushButton*>(QStringLiteral("importAudioBtn"));
  if (importBtn)
    hints->registerTool(importBtn, QStringLiteral("Import Audio"),
                        QStringLiteral("Import an audio file as a sample on "
                                       "the selected track"), QString(),
                        icons::IconId::Folder);
}

} // namespace misuc::ui