#pragma once

#include <QMainWindow>

class QStackedWidget;
class QTimer;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class TransportBar;
class StatusPanel;

// Application shell: transport on top, navigation rail, a stack of real pages
// (Project, Audio), and the status strip.
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(misuc::engine::AudioEngine& engine);
  ~MainWindow() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void buildPages();
  void poll();

  misuc::engine::AudioEngine& engine_;
  QStackedWidget* pages_ = nullptr;
  TransportBar* transport_ = nullptr;
  StatusPanel* status_ = nullptr;
  QTimer* timer_ = nullptr;
  QWidget* navRail_ = nullptr;
};

} // namespace misuc::ui