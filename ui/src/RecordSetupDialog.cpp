#include "RecordSetupDialog.h"

#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

#include "Theme.h"
#include "branding/Icons.h"

namespace misuc::ui {

RecordSetupDialog::RecordSetupDialog(
    const std::vector<misuc::engine::AudioDeviceInfo>& inputDevices,
    const QString& projectPath,
    const std::vector<std::string>& trackNames,
    int currentTrackIndex,
    QWidget* parent)
  : QDialog(parent)
  , inputDevices_(inputDevices)
  , projectPath_(projectPath)
  , trackNames_(trackNames)
  , currentTrackIndex_(currentTrackIndex) {
  setWindowTitle("Record Setup");
  setModal(true);
  setMinimumWidth(520);
  setupUi();
  populateDevices();
  populateTracks();

  // Default file name with counter
  config_.fileName = "Recording_001";
  fileNameEdit_->setText(QString::fromStdString(config_.fileName));

  // Default recording path
  if (!projectPath_.isEmpty()) {
    QDir projectDir(QFileInfo(projectPath_).absolutePath());
    config_.recordPath = projectDir.filePath("Audio Recordings").toStdString();
    QDir().mkpath(QString::fromStdString(config_.recordPath));
  } else {
    config_.recordPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation).toStdString() + "/Misuc Recordings";
    QDir().mkpath(QString::fromStdString(config_.recordPath));
  }
  updatePathDisplay();

  // Start input meter timer if we have input devices
  if (!inputDevices_.empty()) {
    meterTimer_ = new QTimer(this);
    connect(meterTimer_, &QTimer::timeout, this, [this]() {
      // TODO: Connect to actual input meter from engine
      // For now, simulate with random values for demo
      // In real implementation, this would read from the engine's input meter
    });
    meterTimer_->start(50);
  }
}

void RecordSetupDialog::setupUi() {
  auto* mainLayout = new QVBoxLayout(this);
  mainLayout->setSpacing(12);
  mainLayout->setContentsMargins(16, 16, 16, 16);

  // ========== Recording Type ==========
  auto* typeGroup = new QGroupBox("What to Record");
  auto* typeLayout = new QVBoxLayout(typeGroup);
  typeGroup_ = new QButtonGroup(this);

  audioRadio_ = new QRadioButton("Audio");
  audioRadio_->setChecked(true);
  midiRadio_ = new QRadioButton("MIDI / Notes");
  midiRadio_->setEnabled(false); // Not implemented yet
  midiRadio_->setToolTip("MIDI recording not yet implemented");

  typeGroup_->addButton(audioRadio_, 0);
  typeGroup_->addButton(midiRadio_, 1);
  typeLayout->addWidget(audioRadio_);
  typeLayout->addWidget(midiRadio_);
  mainLayout->addWidget(typeGroup);

  // ========== Audio Settings ==========
  auto* audioGroup = new QGroupBox("Audio Settings");
  auto* audioLayout = new QVBoxLayout(audioGroup);
  audioLayout->setSpacing(8);

  // Input Device
  auto* deviceRow = new QHBoxLayout();
  deviceRow->addWidget(new QLabel("Input Device:"));
  deviceCombo_ = new QComboBox();
  deviceCombo_->setMinimumWidth(280);
  deviceRow->addWidget(deviceCombo_, 1);
  audioLayout->addLayout(deviceRow);

  // Input Meter
  auto* meterRow = new QHBoxLayout();
  meterRow->addWidget(new QLabel("Input Level:"));
  inputMeterL_ = new QProgressBar();
  inputMeterL_->setRange(0, 100);
  inputMeterL_->setTextVisible(false);
  inputMeterL_->setFixedHeight(14);
  inputMeterL_->setStyleSheet("QProgressBar::chunk { background: #4ade80; }");
  meterRow->addWidget(inputMeterL_, 1);
  inputMeterR_ = new QProgressBar();
  inputMeterR_->setRange(0, 100);
  inputMeterR_->setTextVisible(false);
  inputMeterR_->setFixedHeight(14);
  inputMeterR_->setStyleSheet("QProgressBar::chunk { background: #4ade80; }");
  meterRow->addWidget(inputMeterR_, 1);
  audioLayout->addLayout(meterRow);

  // Channels
  auto* chRow = new QHBoxLayout();
  chRow->addWidget(new QLabel("Channels:"));
  channelsCombo_ = new QComboBox();
  channelsCombo_->addItem("Mono (1)", 1);
  channelsCombo_->addItem("Stereo (2)", 2);
  channelsCombo_->setCurrentIndex(1);
  chRow->addWidget(channelsCombo_);
  chRow->addStretch();
  audioLayout->addLayout(chRow);

  // Sample Rate
  auto* srRow = new QHBoxLayout();
  srRow->addWidget(new QLabel("Sample Rate:"));
  sampleRateCombo_ = new QComboBox();
  sampleRateCombo_->addItem("44.1 kHz", 44100);
  sampleRateCombo_->addItem("48 kHz", 48000);
  sampleRateCombo_->addItem("96 kHz", 96000);
  sampleRateCombo_->setCurrentIndex(1);
  srRow->addWidget(sampleRateCombo_);
  srRow->addStretch();
  audioLayout->addLayout(srRow);

  mainLayout->addWidget(audioGroup);

  // ========== File Settings ==========
  auto* fileGroup = new QGroupBox("File Settings");
  auto* fileLayout = new QVBoxLayout(fileGroup);
  fileLayout->setSpacing(8);

  // Format
  auto* fmtRow = new QHBoxLayout();
  fmtRow->addWidget(new QLabel("Format:"));
  formatCombo_ = new QComboBox();
  formatCombo_->addItem("WAV (uncompressed)", "wav");
  formatCombo_->addItem("FLAC (lossless)", "flac");
  formatCombo_->setCurrentIndex(0);
  connect(formatCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &RecordSetupDialog::onFormatChanged);
  fmtRow->addWidget(formatCombo_);
  fmtRow->addStretch();
  fileLayout->addLayout(fmtRow);

  // Recording Path
  auto* pathRow = new QHBoxLayout();
  pathRow->addWidget(new QLabel("Folder:"));
  pathEdit_ = new QLineEdit();
  pathEdit_->setReadOnly(true);
  pathRow->addWidget(pathEdit_, 1);
  browseBtn_ = new QPushButton("Browse...");
  connect(browseBtn_, &QPushButton::clicked, this, &RecordSetupDialog::onBrowsePath);
  pathRow->addWidget(browseBtn_);
  fileLayout->addLayout(pathRow);

  // File Name
  auto* nameRow = new QHBoxLayout();
  nameRow->addWidget(new QLabel("File Name:"));
  fileNameEdit_ = new QLineEdit();
  nameRow->addWidget(fileNameEdit_, 1);
  fileLayout->addLayout(nameRow);

  mainLayout->addWidget(fileGroup);

  // ========== Recording Options ==========
  auto* optGroup = new QGroupBox("Recording Options");
  auto* optLayout = new QVBoxLayout(optGroup);
  optLayout->setSpacing(8);

  // Count-in
  auto* ciRow = new QHBoxLayout();
  ciRow->addWidget(new QLabel("Count-in:"));
  countInCombo_ = new QComboBox();
  countInCombo_->addItem("None", 0);
  countInCombo_->addItem("1 Bar", 1);
  countInCombo_->addItem("2 Bars", 2);
  countInCombo_->setCurrentIndex(2);
  ciRow->addWidget(countInCombo_);
  ciRow->addStretch();
  optLayout->addLayout(ciRow);

  // Metronome
  metronomeCheck_ = new QCheckBox("Metronome");
  metronomeCheck_->setChecked(true);
  optLayout->addWidget(metronomeCheck_);

  // Input Monitor
  monitorCheck_ = new QCheckBox("Input Monitor (hear yourself while recording)");
  monitorCheck_->setChecked(true);
  optLayout->addWidget(monitorCheck_);

  mainLayout->addWidget(optGroup);

  // ========== Destination ==========
  auto* destGroup = new QGroupBox("Record Destination");
  auto* destLayout = new QVBoxLayout(destGroup);
  destLayout->setSpacing(8);

  auto* destRow = new QHBoxLayout();
  destRow->addWidget(new QLabel("Record into:"));
  destinationCombo_ = new QComboBox();
  destRow->addWidget(destinationCombo_, 1);
  destLayout->addLayout(destRow);

  newTrackCheck_ = new QCheckBox("Create new track");
  newTrackCheck_->setChecked(true);
  destLayout->addWidget(newTrackCheck_);

  mainLayout->addWidget(destGroup);

  // ========== Buttons ==========
  auto* buttonRow = new QHBoxLayout();
  buttonRow->addStretch();
  cancelBtn_ = new QPushButton("Cancel");
  cancelBtn_->setMinimumWidth(100);
  connect(cancelBtn_, &QPushButton::clicked, this, &QDialog::reject);
  buttonRow->addWidget(cancelBtn_);

  okBtn_ = new QPushButton("Start Recording");
  okBtn_->setMinimumWidth(140);
  okBtn_->setDefault(true);
  okBtn_->setStyleSheet("QPushButton { background: #dc3545; color: white; font-weight: bold; }");
  connect(okBtn_, &QPushButton::clicked, this, &RecordSetupDialog::validateAndAccept);
  buttonRow->addWidget(okBtn_);

  mainLayout->addLayout(buttonRow);
}

void RecordSetupDialog::populateDevices() {
  deviceCombo_->clear();
  deviceCombo_->addItem("System Default", "");
  for (const auto& dev : inputDevices_) {
    if (dev.isInput) {
      deviceCombo_->addItem(QString::fromStdString(dev.name),
                            QString::fromStdString(dev.id));
    }
  }
  if (deviceCombo_->count() <= 1) {
    deviceCombo_->addItem("(No input devices found)", "");
    okBtn_->setEnabled(false);
    okBtn_->setToolTip("No audio input devices available");
  }
}

void RecordSetupDialog::populateTracks() {
  destinationCombo_->clear();
  destinationCombo_->addItem("New Track (auto-create)", -1);
  for (size_t i = 0; i < trackNames_.size(); ++i) {
    destinationCombo_->addItem(QString::fromStdString(trackNames_[i]), (int)i);
  }
  if (currentTrackIndex_ >= 0 && currentTrackIndex_ < (int)trackNames_.size()) {
    destinationCombo_->setCurrentIndex(currentTrackIndex_ + 1);
  }
}

void RecordSetupDialog::updatePathDisplay() {
  pathEdit_->setText(QString::fromStdString(config_.recordPath));
}

void RecordSetupDialog::onFormatChanged(int idx) {
  config_.format = formatCombo_->itemData(idx).toString().toStdString();
  // Update file extension in name if it matches old format
  QString name = fileNameEdit_->text();
  if (name.endsWith(".wav", Qt::CaseInsensitive) || name.endsWith(".flac", Qt::CaseInsensitive)) {
    QString base = name.section('.', 0, -2);
    fileNameEdit_->setText(base + "." + formatCombo_->itemData(idx).toString());
  }
}

void RecordSetupDialog::onBrowsePath() {
  QString dir = QFileDialog::getExistingDirectory(this, "Choose Recording Folder",
                                                   QString::fromStdString(config_.recordPath));
  if (!dir.isEmpty()) {
    config_.recordPath = dir.toStdString();
    updatePathDisplay();
  }
}

void RecordSetupDialog::validateAndAccept() {
  // Validate input device
  if (deviceCombo_->currentData().toString().isEmpty() && deviceCombo_->count() <= 1) {
    QMessageBox::warning(this, "No Input Device", "No audio input device is available. Please connect a microphone or audio interface.");
    return;
  }

  // Validate file name
  QString name = fileNameEdit_->text().trimmed();
  if (name.isEmpty()) {
    QMessageBox::warning(this, "Invalid File Name", "Please enter a file name.");
    return;
  }
  // Remove extension if present
  if (name.contains('.')) {
    name = name.section('.', 0, -2);
  }
  config_.fileName = name.toStdString();

  // Validate recording path
  QDir dir(QString::fromStdString(config_.recordPath));
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      QMessageBox::warning(this, "Invalid Folder", "Cannot create recording folder.");
      return;
    }
  }

  // Collect config
  config_.type = Config::Type::Audio;
  config_.inputDeviceId = deviceCombo_->currentData().toString().toStdString();
  config_.inputDeviceName = deviceCombo_->currentText().toStdString();
  config_.inputChannels = channelsCombo_->currentData().toInt();
  config_.sampleRate = sampleRateCombo_->currentData().toUInt();
  config_.format = formatCombo_->currentData().toString().toStdString();
  config_.countInBars = countInCombo_->currentData().toInt();
  config_.metronomeOn = metronomeCheck_->isChecked();
  config_.inputMonitor = monitorCheck_->isChecked();
  config_.destinationTrack = destinationCombo_->currentData().toInt();
  config_.createNewTrack = newTrackCheck_->isChecked();

  valid_ = true;
  accept();
}

} // namespace misuc::ui