#pragma once

#include <QWidget>

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class HintManager;

// Transport controls: play/stop transport, tempo, count-in toggle, and the
// formatted bar.beat.sixteenth.subtick position readout. Reads atomic playhead
// samples from the engine and renders offline; no real-time work here.
class TransportBar : public QWidget {
  Q_OBJECT
public:
  explicit TransportBar(misuc::engine::AudioEngine& engine, QWidget* parent = nullptr);

  void setCountInOn(bool on);
  bool countInOn() const { return countIn_; }
  void setMetronomeVolume(float v);
  float metronomeVolume() const { return metronomeVol_; }
  void setCountInBeats(float b);
  float countInBeats() const { return countInBeats_; }
  void setLoopOn(bool on);
  void setRecordingState(bool on) { recording_ = on; update(); }
  bool isRecordingState() const { return recording_; }
  void setSnapOn(bool on);
  void setSongMode(bool song);
  bool snapOn() const { return snap_; }
  bool songMode() const { return songMode_; }
  void refresh(); // pull playhead + tempo state

  // Painted control hit-tests (for UI-driven tests of the toolbar buttons).
  QRectF recordButtonRect() const { return recordBtn_; }
  QRectF metronomeButtonRect() const { return clickBtn_; }

  // Hover-intent hints for the painted controls (§78: fully developed hint bar).
  void attachHints(HintManager* hints);

  // Position readout text for the hint bar.
  QString positionText() const;

signals:
  void tempoChanged(double bpm);
  void countInToggled(bool on);
  void loopToggled(bool on);
  void recordRequested();      // Request to open Record Setup dialog
  void recordStateChanged(bool recording); // Recording transport state
  void snapToggled(bool on);
  void modeToggled(bool song);
  void metronomeVolumeChanged(float volume);
  void countInBeatsChanged(float beats);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;

private:
  QRectF buttonRect(const QRectF& playRect, int index) const;

  misuc::engine::AudioEngine& engine_;
  bool playing_ = false;
  bool countIn_ = false;
  float metronomeVol_ = 0.9f;
  float countInBeats_ = 2.0f;
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