#pragma once

#include <QWidget>

#include <misuc/engine/audio/AudioBackend.h>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QCheckBox;
class QTimer;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class MeterWidget;

// Real audio configuration page. Lists devices discovered from the live
// PipeWire backend, lets the user choose sample rate / buffer size, starts and
// stops the device, toggles the 440 Hz test tone, the groove, and input
// monitoring, sets master/monitor gain, and shows live output + input meters,
// latency and xrun state. Everything here is wired to the engine.
class AudioSettingsPage : public QWidget {
  Q_OBJECT
public:
  explicit AudioSettingsPage(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

  // Reopens the last successful device configuration. Used to restore the
  // stream after an offline export that had to stop it.
  void restartStream();

private Q_SLOTS:
  void refreshDevices();
  void openDevice();
  void closeDevice();
  void toggleTestTone();
  void toggleGroove();
  void toggleMonitor();
  void poll();

private:
  void applyConfig();
  bool startWith(const misuc::engine::AudioConfig& cfg);

  misuc::engine::AudioEngine& engine_;

  QComboBox* outputDevices_ = nullptr;
  QComboBox* inputDevices_ = nullptr;
  QComboBox* sampleRate_ = nullptr;
  QComboBox* bufferSize_ = nullptr;
  QCheckBox* monitorChk_ = nullptr;
  QCheckBox* grooveChk_ = nullptr;

  QPushButton* openBtn_ = nullptr;
  QPushButton* toneBtn_ = nullptr;
  QLabel* statusLabel_ = nullptr;
  QLabel* latencyLabel_ = nullptr;
  QLabel* deviceInfo_ = nullptr;
  QSlider* gainSlider_ = nullptr;
  QLabel* gainValue_ = nullptr;
  QSlider* monitorSlider_ = nullptr;
  QLabel* monitorValue_ = nullptr;
  MeterWidget* meter_ = nullptr;
  MeterWidget* inMeter_ = nullptr;
  QTimer* timer_ = nullptr;

  bool toneOn_ = false;
  bool grooveOn_ = false;
  bool monitorOn_ = false;

  bool lastConfigValid_ = false;
  misuc::engine::AudioConfig lastConfig_;
};

} // namespace misuc::ui