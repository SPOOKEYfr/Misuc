#pragma once

#include <QWidget>

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

// Transport controls: play/stop transport, tempo, count-in toggle, and the
// formatted bar.beat.sixteenth.subtick position readout. Reads atomic playhead
// samples from the engine and renders offline; no real-time work here.
class TransportBar : public QWidget {
  Q_OBJECT
public:
  explicit TransportBar(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

  void setCountInOn(bool on);
  void setLoopOn(bool on);
  void setRecording(bool on) { recording_ = on; update(); }
  void setSnapOn(bool on);
  void setSongMode(bool song);
  bool snapOn() const { return snap_; }
  bool songMode() const { return songMode_; }
  void refresh(); // pull playhead + tempo state

signals:
  void tempoChanged(double bpm);
  void countInToggled(bool on);
  void loopToggled(bool on);
  void recordToggled(bool on);
  void snapToggled(bool on);
  void modeToggled(bool song);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;

private:
  QRectF buttonRect(const QRectF& playRect, int index) const;

  misuc::engine::AudioEngine& engine_;
  bool playing_ = false;
  bool countIn_ = false;
  bool loopOn_ = false;
  bool recording_ = false;
  bool snap_ = true;
  bool songMode_ = true;
  uint64_t playhead_ = 0;
  double tempo_ = 120.0;
  int tsNum_ = 4;
  int tsDen_ = 4;

  QRectF playBtn_;
  QRectF stopBtn_;
  QRectF clickBtn_;
  QRectF loopBtn_;
  QRectF recordBtn_;
  QRectF snapBtn_;
  QRectF modeBtn_;
  QRectF displayRect_;
};

} // namespace misuc::ui