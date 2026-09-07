#pragma once

#include <QWidget>

namespace misuc::ui {

// Vertical two-bar level meter (L/R) for mixer channel strips: RMS fill,
// peak cap with slow peak-hold, and a clip LED. Painted from theme tokens and
// polled at UI rate; purely paint-side like the transport meter.
class VerticalMeter : public QWidget {
  Q_OBJECT
public:
  explicit VerticalMeter(QWidget* parent = nullptr);

  void setLevels(float peakL, float rmsL, float peakR, float rmsR,
                 bool clipped);
  void clear() { setLevels(0.f, 0.f, 0.f, 0.f, false); }

  QSize sizeHint() const override { return QSize(26, 220); }
  QSize minimumSizeHint() const override { return QSize(24, 80); }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void drawBar(QPainter& p, const QRectF& bar, float level, float& hold,
               float rms);

  float peakL_ = 0.f, rmsL_ = 0.f, peakR_ = 0.f, rmsR_ = 0.f;
  float holdL_ = 0.f, holdR_ = 0.f;
  bool clipped_ = false;
};

} // namespace misuc::ui