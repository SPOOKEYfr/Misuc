#pragma once

#include <QElapsedTimer>
#include <QToolBar>

class QLabel;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class MiniMeters;
class TransportBar;

// Global application header: brand cluster (mark + wordmark + project name) on
// the left, the transport centred, and a live status cluster on the right
// (app CPU/RAM, engine rate, buffer+latency, xruns, audio state, master mini
// meters). Polled from the UI timer via refresh().
class HeaderBar : public QToolBar {
  Q_OBJECT
public:
  explicit HeaderBar(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

  TransportBar* transportBar() const { return transport_; }
  QLabel* navLogo() const { return navLogo_; }

  void refresh();                       // status + meters + cpu/ram
  void setProjectName(const QString& name);

private:
  QWidget* buildLeftCluster();
  QWidget* buildCenterTransport();
  QWidget* buildRightCluster();
  void refreshCpuRam();
  void refreshLogo();

  misuc::engine::AudioEngine& engine_;
  QLabel* navLogo_ = nullptr;
  QLabel* wordmark_ = nullptr;
  QLabel* projectName_ = nullptr;
  TransportBar* transport_ = nullptr;
  QLabel* cpuLabel_ = nullptr;
  QLabel* ramLabel_ = nullptr;
  QLabel* srLabel_ = nullptr;
  QLabel* latencyLabel_ = nullptr;
  QLabel* xrunLabel_ = nullptr;
  QLabel* stateLabel_ = nullptr;
  MiniMeters* meters_ = nullptr;
  long clockTicks_ = 100;
  QElapsedTimer cpuTimer_;
  uint64_t cpuLastJiffies_ = 0;
};

} // namespace misuc::ui