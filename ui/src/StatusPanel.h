#pragma once

#include <QWidget>

class QLabel;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

// Thin status strip at the bottom of the window: engine state, negotiated
// audio, xruns and the wall clock.
class StatusPanel : public QWidget {
  Q_OBJECT
public:
  explicit StatusPanel(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

  void refresh();

private:
  misuc::engine::AudioEngine& engine_;
  QLabel* left_ = nullptr;
  QLabel* right_ = nullptr;
};

} // namespace misuc::ui