#include "Browser.h"

#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QSet>
#include <QDirIterator>
#include <cmath>

#include <sndfile.h>
#include <misuc/engine/audio/WavFile.h>
#include "Theme.h"
#include "branding/Icons.h"

namespace misuc::ui {

namespace {
struct BuiltinEntry { const char* name; const char* tag; };
const BuiltinEntry kInstruments[] = {
    {"Kick Drum", "Synth"},           {"Snare Drum", "Transient"},
    {"Hi-Hat", "Noise"},              {"Bass", "Synth"},
    {"Pluck Lead", "Synth"},          {"Pad", "Synth"},
    {"Acoustic Piano", "Sampled"},    {"Electric Piano", "Sampled"},
    {"Strings", "Sampled"},           {"Brass", "Sampled"},
    {"FM Synth", "Synth"},            {"808 Sub", "Synth"},
};
const BuiltinEntry kEffects[] = {
    {"EQ (4-band)", "Filter"},        {"Compressor", "Dynamics"},
    {"Limiter", "Dynamics"},          {"Reverb (Hall)", "Space"},
    {"Delay (Tempo)", "Time"},        {"Chorus", "Mod"},  {"Tremolo", "Mod"},
    {"Distortion", "Drive"},          {"Overdrive", "Drive"},
    {"Pitch Shifter", "Time"},        {"Stereo Widener", "Width"},
    {"Vocoder", "Filter"},
};
const BuiltinEntry kMidi[] = {
    {"MIDI Clip / Regions", "IDE"},
    {"MIDI Remote Script", "Control"},
    {"Hardware MIDI Device", "I/O"},
    {"MIDI Storm Pattern", "IDE"},
};
} // namespace

namespace {
const BuiltinEntry kPluginsList[] = {
    {"LADSPA Host", "Plugin"},
    {"LV2 Host (scan)", "Plugin"},
    {"VST3 Host (optional)", "Plugin"},
};
}

Browser::Browser(QWidget* parent) : QWidget(parent) {
  setMinimumSize(420, 260);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* split = new QSplitter(Qt::Horizontal, this);
  split->setChildrenCollapsible(false);
  outer->addWidget(split);

  cats_ = new QTreeWidget(split);
  cats_->setHeaderHidden(true);
  cats_->setFocusPolicy(Qt::NoFocus);
  cats_->setStyleSheet(QString("background: %1;").arg(theme::kPanel.name()));
  split->addWidget(cats_);

  auto* rightCol = new QWidget(split);
  auto* rcLay = new QVBoxLayout(rightCol);
  rcLay->setContentsMargins(6, 6, 6, 6);
  rcLay->setSpacing(6);

  auto* searchHeader = new QHBoxLayout;
  auto* search = new QLineEdit(rightCol);
  search->setPlaceholderText("Search samples...");
  search->setClearButtonEnabled(true);
  auto* searchIcon = new QLabel(rightCol);
  searchIcon->setPixmap(icons::make(icons::IconId::Search, theme::kTextDim, 16).pixmap(16, 16));
  searchHeader->addWidget(searchIcon);
  searchHeader->addWidget(search, 1);
  rcLay->addLayout(searchHeader);

  files_ = new QTreeWidget(rightCol);
  files_->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Tag")});
  files_->setRootIsDecorated(false);
  files_->setFocusPolicy(Qt::NoFocus);
  files_->setDragDropMode(QAbstractItemView::DragOnly);
  files_->header()->setStretchLastSection(true);
  files_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  files_->setStyleSheet(QString("background: %1;").arg(theme::kPanel.name()));
  rcLay->addWidget(files_, 1);

  // bottom preview strip
  auto* preview = new QWidget(rightCol);
  preview->setStyleSheet(QString("background: %1; border-top: 1px solid %2;")
      .arg(theme::kPanelRaised.name(), theme::kBorder.name()));
  previewHost_ = new QVBoxLayout(preview);
  previewHost_->setContentsMargins(8, 6, 8, 6);
  previewTitle_ = new QLabel("Select an audio file to preview", preview);
  previewTitle_->setStyleSheet("color: " + theme::kTextFaint.name());
  previewMeta_ = new QLabel(preview);
  previewMeta_->setStyleSheet("color: " + theme::kTextDim.name());
  previewHost_->addWidget(previewTitle_);
  previewHost_->addWidget(previewMeta_);
  auto* btnRow = new QHBoxLayout;
  auto* playBtn = new QPushButton("Preview", preview);
  auto* insertBtn = new QPushButton("Insert", preview);
  btnRow->addWidget(playBtn);
  btnRow->addWidget(insertBtn);
  btnRow->addStretch(1);
  previewHost_->addLayout(btnRow);
  rcLay->addWidget(preview);

  split->addWidget(rightCol);
  split->setStretchFactor(0, 1);
  split->setStretchFactor(1, 3);
  split->setSizes({150, 360});

  // categories
  auto* allC  = new QTreeWidgetItem(cats_, {QStringLiteral("All Samples")});
  auto* curC  = new QTreeWidgetItem(cats_, {QStringLiteral("Current Project")});
  auto* insC  = new QTreeWidgetItem(cats_, {QStringLiteral("Instruments")});
  auto* efxC  = new QTreeWidgetItem(cats_, {QStringLiteral("Audio Effects")});
  auto* midiC = new QTreeWidgetItem(cats_, {QStringLiteral("MIDI")});
  auto* plgC  = new QTreeWidgetItem(cats_, {QStringLiteral("Plugins")});
  allC->setIcon(0, icons::make(icons::IconId::Audio, theme::kTextDim, 16));
  curC->setIcon(0, icons::make(icons::IconId::CurrentProject, theme::kTextDim, 16));
  insC->setIcon(0, icons::make(icons::IconId::Instruments, theme::kTextDim, 16));
  efxC->setIcon(0, icons::make(icons::IconId::Effects, theme::kTextDim, 16));
  midiC->setIcon(0, icons::make(icons::IconId::Midi, theme::kTextDim, 16));
  plgC->setIcon(0, icons::make(icons::IconId::Plugins, theme::kTextDim, 16));
  cats_->expandAll();
  cats_->setCurrentItem(allC);

  connect(search, &QLineEdit::textChanged, this, [this](const QString& t) {
    for (int i = 0; i < files_->topLevelItemCount(); ++i) {
      auto* item = files_->topLevelItem(i);
      item->setHidden(!(t.isEmpty() ||
                        item->text(0).contains(t, Qt::CaseInsensitive)));
    }
  });

  connect(cats_, &QTreeWidget::currentItemChanged, this,
          [this] { onCategoryActivated(); });
  connect(files_, &QTreeWidget::currentItemChanged, this,
          [this](QTreeWidgetItem* cur) { selectFileItem(cur); });
  connect(files_, &QTreeWidget::itemDoubleClicked, this,
          [this] { onFileActivated(); });
  connect(playBtn, &QPushButton::clicked, this, [this] {
    if (files_->currentItem()) {
      QString p = files_->currentItem()->data(0, Qt::UserRole).toString();
      if (!p.isEmpty()) buildPreview(p);
    }
  });
  connect(insertBtn, &QPushButton::clicked, this,
          [this] {
            if (files_->currentItem()) {
              QString p = files_->currentItem()->data(0, Qt::UserRole).toString();
              if (!p.isEmpty()) emit insertSample(p);
            }
          });
  onCategoryActivated();
}

void Browser::onCategoryActivated() {
  auto* cur = cats_->currentItem();
  if (!cur) return;
  files_->clear();
  const QString cat = cur->text(0);

  if (cat == "Instruments" || cat == "Audio Effects" || cat == "MIDI" ||
      cat == "Plugins") {
    const BuiltinEntry* list = nullptr;
    int n = 0;
    if (cat == "Instruments") { list = kInstruments;  n = int(sizeof kInstruments / sizeof *kInstruments); }
    if (cat == "Audio Effects") { list = kEffects;     n = int(sizeof kEffects / sizeof *kEffects); }
    if (cat == "MIDI")          { list = kMidi;        n = int(sizeof kMidi / sizeof *kMidi); }
    if (cat == "Plugins")       { list = kPluginsList; n = int(sizeof kPluginsList / sizeof *kPluginsList); }
    for (int i = 0; i < n; ++i) {
      auto* it = new QTreeWidgetItem(files_);
      it->setText(0, list[i].name);
      it->setText(1, list[i].tag);
      it->setIcon(0, icons::make(
          cat == "Instruments" ? icons::IconId::Instruments
          : cat == "Audio Effects" ? icons::IconId::Effects
          : cat == "MIDI" ? icons::IconId::Midi : icons::IconId::Plugins,
          theme::kTextDim, 14));
    }
    previewTitle_->clear();
    previewMeta_->clear();
    return;
  }

  // Audio categories: real files
  QSet<QString> audioExt = { "wav", "flac", "ogg", "mp3" };
  QStringList baseDirs;
  QString musicDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
  if (!musicDir.isEmpty()) baseDirs << musicDir;
  QDir home = QDir::home();
  baseDirs << home.absoluteFilePath("Music")
           << home.absoluteFilePath("Samples")
           << home.absoluteFilePath("Sounds");

  QStringList names;
  if (baseDirs.isEmpty()) baseDirs << QDir::homePath();
  for (auto& dir : baseDirs) {
    QDirIterator it(dir, QDir::Files | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && names.size() < 4000) {
      QString f = it.next();
      if (audioExt.contains(QFileInfo(f).suffix().toLower())) names << f;
    }
  }
  if (cat == "Current Project") names = sessionSources_;

  for (auto& f : names) {
    QFileInfo fi(f);
    auto* it = new QTreeWidgetItem(files_);
    it->setText(0, fi.fileName());
    it->setText(1, QString("%1 KB").arg(qMax(1, int(fi.size() / 1024))));
    it->setIcon(0, icons::make(icons::IconId::Audio, theme::kTextDim, 14));
    it->setData(0, Qt::UserRole, f);
  }
  previewTitle_->setText(QString::number(files_->topLevelItemCount()) + " audio files");
  previewMeta_->clear();
}

void Browser::selectFileItem(QTreeWidgetItem* cur) {
  if (!cur) return;
  QString p = cur->data(0, Qt::UserRole).toString();
  if (!p.isEmpty()) buildPreview(p);
}

void Browser::onFileActivated() {
  auto* cur = files_->currentItem();
  if (!cur) return;
  QString p = cur->data(0, Qt::UserRole).toString();
  if (!p.isEmpty()) emit insertSample(p);
}

void Browser::buildPreview(const QString& path) {
  QFileInfo fi(path);
  if (!fi.exists() || !fi.isReadable()) {
    previewTitle_->setText(fi.fileName() + " (unreadable)");
    previewMeta_->setText("The preview could not be decoded.");
    return;
  }
  misuc::audio::AudioBuffer audio;
  std::string err;
  if (!misuc::audio::readAudioFile(path.toStdString(), audio, err)) {
    previewTitle_->setText(fi.fileName() + " (decode failed)");
    previewMeta_->setText(QString::fromStdString(err));
    return;
  }
  previewTitle_->setText(fi.fileName());
  QString meta = QString("WAV · %1 ch · %2 Hz · %3 s")
      .arg(audio.channels)
      .arg(qint64(audio.sampleRate))
      .arg(audio.durationSeconds(), 0, 'f', 2);
  previewMeta_->setText(meta);
}

void Browser::setSessionSources(const QStringList& sourceFiles) {
  sessionSources_ = sourceFiles;
  if (cats_->currentItem() && cats_->currentItem()->text(0) == "Current Project")
    onCategoryActivated();
}

void Browser::refresh() {
  if (cats_->currentItem()) onCategoryActivated();
}

} // namespace misuc::ui