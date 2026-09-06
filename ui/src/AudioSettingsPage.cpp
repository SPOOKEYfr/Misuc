#include "AudioSettingsPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QTimer>
#include <QGroupBox>

#include <misuc/engine/audio/AudioEngine.h>
#include <misuc/engine/audio/AudioBackend.h>

#include "MeterWidget.h"
#include "Theme.h"

namespace misuc::ui {

namespace {
QWidget* header(QWidget* parent, const QString& title, const QString& subtitle = {}) {
  auto* box = new QWidget(parent);
  auto* l = new QVBoxLayout(box);
  l->setContentsMargins(0, 0, 0, 8);
  auto* t = new QLabel(title, box);
  const QFont f = theme::uiFont(13, QFont::Bold);
  t->setFont(f);
  t->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kText.name()));
  l->addWidget(t);
  if (!subtitle.isEmpty()) {
    auto* s = new QLabel(subtitle, box);
    s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    s->setWordWrap(true);
    l->addWidget(s);
  }
  return box;
}
} // namespace

AudioSettingsPage::AudioSettingsPage(misuc::engine::AudioEngine& engine,
                                     QWidget* parent)
    : QWidget(parent), engine_(engine) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 24);
  root->setSpacing(16);

  root->addWidget(header(this, "Audio Setup",
                         "Pick your output device and buffer size, then apply. "
                         "A 440 Hz test tone verifies the path."));

  auto* grid = new QGridLayout();
  grid->setHorizontalSpacing(24);

  auto* devGroup = new QGroupBox(QStringLiteral("Device"), this);
  auto* devForm = new QFormLayout(devGroup);
  outputDevices_ = new QComboBox(devGroup);
  inputDevices_ = new QComboBox(devGroup);
  inputDevices_->setEnabled(false); // playback is the verified path for now
  devForm->addRow(QStringLiteral("Output"), outputDevices_);
  devForm->addRow(QStringLiteral("Input"), inputDevices_);
  grid->addWidget(devGroup, 0, 0);

  auto* bufGroup = new QGroupBox(QStringLiteral("Configuration"), this);
  auto* bufForm = new QFormLayout(bufGroup);
  sampleRate_ = new QComboBox(bufGroup);
  bufferSize_ = new QComboBox(bufGroup);
  for (uint32_t sr : engine_.backend()->supportedSampleRates())
    sampleRate_->addItem(QStringLiteral("%1 Hz").arg(sr), sr);
  for (uint32_t bs : engine_.backend()->supportedBufferSizes())
    bufferSize_->addItem(QStringLiteral("%1 frames").arg(bs), bs);
  sampleRate_->setCurrentIndex(3); // 48000
  bufferSize_->setCurrentIndex(2); // 256
  bufForm->addRow(QStringLiteral("Sample rate"), sampleRate_);
  bufForm->addRow(QStringLiteral("Buffer size"), bufferSize_);
  grid->addWidget(bufGroup, 0, 1);

  auto* masterGroup = new QGroupBox(QStringLiteral("Master"), this);
  auto* masterForm = new QFormLayout(masterGroup);
  gainSlider_ = new QSlider(Qt::Horizontal, masterGroup);
  gainSlider_->setRange(-60, 0);
  gainSlider_->setValue(0);
  gainValue_ = new QLabel(QStringLiteral("0.0 dB"), masterGroup);
  gainValue_->setStyleSheet(QStringLiteral("color: %1;")
                                .arg(theme::kText.name()));
  connect(gainSlider_, &QSlider::valueChanged, this, [this](int v) {
    engine_.setMasterGainDb(static_cast<float>(v));
    gainValue_->setText(QStringLiteral("%1 dB").arg(v));
  });
  masterForm->addRow(QStringLiteral("Gain"), gainSlider_);
  masterForm->addRow(QStringLiteral(""), gainValue_);
  grid->addWidget(masterGroup, 0, 2);

  root->addLayout(grid);

  // Status / meter row.
  auto* statusRow = new QHBoxLayout();
  statusRow->setSpacing(24);
  auto* infoCol = new QVBoxLayout();
  statusLabel_ = new QLabel(QStringLiteral("Engine stopped."), this);
  statusLabel_->setWordWrap(true);
  statusLabel_->setStyleSheet(QStringLiteral("color: %1;")
                                  .arg(theme::kTextDim.name()));
  latencyLabel_ = new QLabel(this);
  latencyLabel_->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(theme::kTextFaint.name()));
  deviceInfo_ = new QLabel(this);
  deviceInfo_->setWordWrap(true);
  deviceInfo_->setStyleSheet(QStringLiteral("color: %1;")
                                 .arg(theme::kTextFaint.name()));
  infoCol->addWidget(statusLabel_);
  infoCol->addWidget(latencyLabel_);
  infoCol->addWidget(deviceInfo_);
  infoCol->addStretch();

  meter_ = new MeterWidget(this);
  statusRow->addLayout(infoCol, 1);
  statusRow->addWidget(meter_, 0, Qt::AlignRight);

  auto* meterCaption = new QLabel(QStringLiteral("Output level"), this);
  meterCaption->setStyleSheet(QStringLiteral("color: %1;")
                                  .arg(theme::kTextFaint.name()));
  auto* meterCol = new QVBoxLayout();
  meterCol->addLayout(statusRow, 1);
  meterCol->addWidget(meterCaption, 0, Qt::AlignLeft);
  root->addLayout(meterCol);

  // Action buttons.
  auto* actions = new QHBoxLayout();
  openBtn_ = new QPushButton(QStringLiteral("Apply & Start"), this);
  openBtn_->setMinimumWidth(140);
  toneBtn_ = new QPushButton(QStringLiteral("Play Test Tone"), this);
  toneBtn_->setCheckable(true);
  toneBtn_->setEnabled(false);
  actions->addWidget(openBtn_);
  actions->addWidget(toneBtn_);
  actions->addStretch();
  root->addLayout(actions);

  root->addStretch();

  connect(openBtn_, &QPushButton::clicked, this, &AudioSettingsPage::openDevice);
  connect(toneBtn_, &QPushButton::toggled, this, &AudioSettingsPage::toggleTestTone);

  timer_ = new QTimer(this);
  timer_->setInterval(33);
  connect(timer_, &QTimer::timeout, this, &AudioSettingsPage::poll);
  timer_->start();
  refreshDevices();
}

void AudioSettingsPage::refreshDevices() {
  const bool wasEnabled = openBtn_->isEnabled();
  outputDevices_->clear();
  for (const auto& d : engine_.backend()->enumerateDevices(false))
    outputDevices_->addItem(QString::fromStdString(d.name),
                            QString::fromStdString(d.id));
  openBtn_->setEnabled(wasEnabled);
}

void AudioSettingsPage::applyConfig() {
  misuc::engine::AudioConfig cfg;
  cfg.deviceId = outputDevices_->currentData().toString().toStdString();
  cfg.sampleRate = sampleRate_->currentData().toUInt();
  cfg.bufferSize = bufferSize_->currentData().toUInt();
  std::string err;
  engine_.closeAudio();
  if (!engine_.open(cfg, err)) {
    statusLabel_->setText(QStringLiteral("Open failed: %1").arg(
        QString::fromStdString(err)));
    return;
  }
  if (!engine_.start()) {
    statusLabel_->setText(QStringLiteral("Start failed: %1").arg(
        QString::fromStdString(engine_.backendStatus().error)));
    return;
  }
  statusLabel_->setText(QStringLiteral("Engine running."));
  toneBtn_->setEnabled(true);
}

void AudioSettingsPage::openDevice() { applyConfig(); }

void AudioSettingsPage::closeDevice() {
  engine_.closeAudio();
  statusLabel_->setText(QStringLiteral("Engine stopped."));
  toneBtn_->setEnabled(false);
  toneBtn_->setChecked(false);
}

void AudioSettingsPage::toggleTestTone() {
  toneOn_ = toneBtn_->isChecked();
  engine_.setTestTone(toneOn_);
}

void AudioSettingsPage::poll() {
  const misuc::engine::BackendStatus st = engine_.backendStatus();
  const misuc::engine::LatencyInfo lat = engine_.latency();

  if (st.running) {
    statusLabel_->setText(QStringLiteral("Engine running on %1.")
                              .arg(QString::fromStdString(
                                  engine_.backend()->backendName())));
    latencyLabel_->setText(
        QStringLiteral("Buffer %1 ms | Output %2 ms | Round-trip %3 ms | %4 xruns")
            .arg(lat.bufferMs, 0, 'f', 1)
            .arg(lat.outputMs, 0, 'f', 1)
            .arg(lat.roundTripMs, 0, 'f', 1)
            .arg(st.xruns));
    deviceInfo_->setText(
        QStringLiteral("%1 Hz / %2 ch / %3 frames")
            .arg(st.negotiatedRate)
            .arg(st.negotiatedChannels)
            .arg(st.negotiatedBuffer));
    meter_->setLevels(engine_.meterPeakL(), engine_.meterRmsL(),
                      engine_.meterPeakR(), engine_.meterRmsR(),
                      engine_.meterClipped());
    toneBtn_->setEnabled(true);
  } else {
    latencyLabel_->setText(QString());
    deviceInfo_->setText(QString());
  }
}

} // namespace misuc::ui