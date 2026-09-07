#include "AudioSettingsPage.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSlider>
#include <QCheckBox>
#include <QTimer>
#include <QGroupBox>
#include <QButtonGroup>

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
                         "Pick your devices and buffer size, then apply. "
                         "A 440 Hz test tone verifies the path, and the groove "
                         "beat plays while the transport runs."));

  auto* grid = new QGridLayout();
  grid->setHorizontalSpacing(24);

  auto* devGroup = new QGroupBox(QStringLiteral("Device"), this);
  auto* devForm = new QFormLayout(devGroup);
  outputDevices_ = new QComboBox(devGroup);
  inputDevices_ = new QComboBox(devGroup);
  inputDevices_->addItem(QStringLiteral("None"), QString());
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
  auto* masterLayout = new QVBoxLayout(masterGroup);
  auto* masterForm = new QFormLayout();
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

  monitorSlider_ = new QSlider(Qt::Horizontal, masterGroup);
  monitorSlider_->setRange(-60, 6);
  monitorSlider_->setValue(-12);
  monitorValue_ = new QLabel(QStringLiteral("-12.0 dB"), masterGroup);
  monitorValue_->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(theme::kText.name()));
  connect(monitorSlider_, &QSlider::valueChanged, this, [this](int v) {
    engine_.setMonitorGainDb(static_cast<float>(v));
    monitorValue_->setText(QStringLiteral("%1 dB").arg(v));
  });
  masterForm->addRow(QStringLiteral("Monitor gain"), monitorSlider_);
  masterForm->addRow(QStringLiteral(""), monitorValue_);

  monitorChk_ = new QCheckBox(QStringLiteral("Monitor input"), masterGroup);
  monitorChk_->setChecked(true);
  connect(monitorChk_, &QCheckBox::toggled, this,
          &AudioSettingsPage::toggleMonitor);
  masterLayout->addLayout(masterForm);
  masterLayout->addWidget(monitorChk_);
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

  auto* meterCaption = new QLabel(QStringLiteral("Output level"), this);
  meterCaption->setStyleSheet(QStringLiteral("color: %1;")
                                  .arg(theme::kTextFaint.name()));
  auto* inMeterCaption = new QLabel(QStringLiteral("Input level"), this);
  inMeterCaption->setStyleSheet(QStringLiteral("color: %1;")
                                    .arg(theme::kTextFaint.name()));
  inMeter_ = new MeterWidget(this);
  auto* meterRowInner = new QHBoxLayout();
  meterRowInner->setSpacing(16);
  auto* outCol = new QVBoxLayout();
  outCol->setSpacing(4);
  outCol->addWidget(meterCaption);
  outCol->addWidget(meter_);
  auto* inCol = new QVBoxLayout();
  inCol->setSpacing(4);
  inCol->addWidget(inMeterCaption);
  inCol->addWidget(inMeter_);
  meterRowInner->addLayout(outCol);
  meterRowInner->addLayout(inCol);

  auto* meterCol = new QVBoxLayout();
  meterCol->addLayout(statusRow, 1);
  meterCol->addLayout(meterRowInner);
  root->addLayout(meterCol);

  // Action buttons.
  auto* actions = new QHBoxLayout();
  openBtn_ = new QPushButton(QStringLiteral("Apply & Start"), this);
  openBtn_->setMinimumWidth(140);
  toneBtn_ = new QPushButton(QStringLiteral("Play Test Tone"), this);
  toneBtn_->setCheckable(true);
  toneBtn_->setEnabled(false);
  grooveChk_ = new QCheckBox(QStringLiteral("Groove"), this);
  grooveChk_->setToolTip(
      QStringLiteral("Play a drum+bass groove while the transport runs"));
  connect(grooveChk_, &QCheckBox::toggled, this,
          &AudioSettingsPage::toggleGroove);
  actions->addWidget(openBtn_);
  actions->addWidget(toneBtn_);
  actions->addWidget(grooveChk_);
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
  const QString curOut = outputDevices_->currentData().toString();
  const QString curIn = inputDevices_->currentData().toString();
  outputDevices_->clear();
  for (const auto& d : engine_.backend()->enumerateDevices(false))
    outputDevices_->addItem(QString::fromStdString(d.name),
                            QString::fromStdString(d.id));
  inputDevices_->clear();
  inputDevices_->addItem(QStringLiteral("None"), QString());
  for (const auto& d : engine_.backend()->enumerateDevices(true))
    inputDevices_->addItem(QString::fromStdString(d.name),
                           QString::fromStdString(d.id));
  const int oi = outputDevices_->findData(curOut);
  if (oi >= 0) outputDevices_->setCurrentIndex(oi);
  const int ii = inputDevices_->findData(curIn);
  if (ii >= 0) inputDevices_->setCurrentIndex(ii);
  openBtn_->setEnabled(wasEnabled);
}

void AudioSettingsPage::applyConfig() {
  misuc::engine::AudioConfig cfg;
  cfg.deviceId = outputDevices_->currentData().toString().toStdString();
  cfg.inputDeviceId =
      inputDevices_->currentData().toString().toStdString();
  cfg.sampleRate = sampleRate_->currentData().toUInt();
  cfg.bufferSize = bufferSize_->currentData().toUInt();
  cfg.inputChannels = 2;
  cfg.monitorGainDb = static_cast<float>(monitorSlider_->value());
  if (startWith(cfg)) {
    lastConfig_ = cfg;
    lastConfigValid_ = true;
  }
}

bool AudioSettingsPage::startWith(const misuc::engine::AudioConfig& cfg) {
  std::string err;
  engine_.closeAudio();
  if (!engine_.open(cfg, err)) {
    statusLabel_->setText(QStringLiteral("Open failed: %1").arg(
        QString::fromStdString(err)));
    return false;
  }
  if (!engine_.start()) {
    statusLabel_->setText(QStringLiteral("Start failed: %1").arg(
        QString::fromStdString(engine_.backendStatus().error)));
    engine_.closeAudio();
    return false;
  }
  statusLabel_->setText(QStringLiteral("Engine running."));
  toneBtn_->setEnabled(true);
  engine_.setGroove(grooveChk_->isChecked());
  engine_.setMonitorGainDb(cfg.monitorGainDb);
  return true;
}

void AudioSettingsPage::restartStream() {
  if (!lastConfigValid_)
    return;
  startWith(lastConfig_);
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

void AudioSettingsPage::toggleGroove() {
  grooveOn_ = grooveChk_->isChecked();
  engine_.setGroove(grooveOn_);
}

void AudioSettingsPage::toggleMonitor() {
  monitorOn_ = monitorChk_->isChecked();
  if (!monitorOn_) engine_.setMonitorGainDb(-96.f);
  else engine_.setMonitorGainDb(static_cast<float>(monitorSlider_->value()));
}

void AudioSettingsPage::poll() {
  const misuc::engine::BackendStatus st = engine_.backendStatus();
  const misuc::engine::LatencyInfo lat = engine_.latency();

  if (st.running) {
    statusLabel_->setText(QStringLiteral("Engine running on %1.")
                              .arg(QString::fromStdString(
                                  engine_.backend()->backendName())));
    latencyLabel_->setText(
        QStringLiteral(
            "Buffer %1 ms | Output %2 ms | Input %3 ms | Round-trip %4 ms | "
            "%5 xruns")
            .arg(lat.bufferMs, 0, 'f', 1)
            .arg(lat.outputMs, 0, 'f', 1)
            .arg(lat.inputMs, 0, 'f', 1)
            .arg(lat.roundTripMs, 0, 'f', 1)
            .arg(st.xruns));
    deviceInfo_->setText(
        QStringLiteral("%1 Hz / %2 out / %3 in / %4 frames")
            .arg(st.negotiatedRate)
            .arg(st.negotiatedChannels)
            .arg(st.negotiatedInputChannels)
            .arg(st.negotiatedBuffer));
    meter_->setLevels(engine_.meterPeakL(), engine_.meterRmsL(),
                      engine_.meterPeakR(), engine_.meterRmsR(),
                      engine_.meterClipped());
    inMeter_->setLevels(engine_.inMeterPeak(), engine_.inMeterRms(),
                        engine_.inMeterPeak(), engine_.inMeterRms(),
                        engine_.inMeterClipped());
    toneBtn_->setEnabled(true);
  } else {
    latencyLabel_->setText(QString());
    deviceInfo_->setText(QString());
    meter_->clear();
    inMeter_->clear();
  }
}

} // namespace misuc::ui