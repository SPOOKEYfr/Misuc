#pragma once

#include <QWidget>

namespace misuc::ui {

// Stereo level meter: RMS fill, peak cap with peak-hold marker, and a clip
// LED. Polled from the UI timer; purely paint-side.
class MeterWidget : public QWidget {
  Q_OBJECT
public:
  explicit MeterWidget(QWidget* parent = nullptr);

  void setLevels(float peakL, float rmsL, float peakR, float rmsR, bool clipped);

  void clear() { setLevels(0.f, 0.f, 0.f, 0.f, false); }

  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void drawChannel(QPainter& p, const QRectF& bar, float peak, float rms,
                   float& hold);

  float peakL_ = 0.f, rmsL_ = 0.f;
  float peakR_ = 0.f, rmsR_ = 0.f;
  float holdL_ = 0.f, holdR_ = 0.f;
  bool clipped_ = false;
};

} // namespace misuc::ui