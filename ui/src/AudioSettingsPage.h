#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class MeterWidget;

// Real audio configuration page. Lists devices discovered from the live
// PipeWire backend, lets the user choose sample rate / buffer size, starts and
// stops the device, toggles the 440 Hz test tone, sets master gain, and shows
// live meters, latency and xrun state. Everything here is wired to the engine.
class AudioSettingsPage : public QWidget {
  Q_OBJECT
public:
  explicit AudioSettingsPage(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

private Q_SLOTS:
  void refreshDevices();
  void openDevice();
  void closeDevice();
  void toggleTestTone();
  void poll();

private:
  void applyConfig();

  misuc::engine::AudioEngine& engine_;

  QComboBox* outputDevices_ = nullptr;
  QComboBox* inputDevices_ = nullptr;
  QComboBox* sampleRate_ = nullptr;
  QComboBox* bufferSize_ = nullptr;

  QPushButton* openBtn_ = nullptr;
  QPushButton* toneBtn_ = nullptr;
  QLabel* statusLabel_ = nullptr;
  QLabel* latencyLabel_ = nullptr;
  QLabel* deviceInfo_ = nullptr;
  QSlider* gainSlider_ = nullptr;
  QLabel* gainValue_ = nullptr;
  MeterWidget* meter_ = nullptr;
  QTimer* timer_ = nullptr;

  bool toneOn_ = false;
};

} // namespace misuc::ui