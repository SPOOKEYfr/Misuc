#include "VerticalMeter.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

#include "../Theme.h"

namespace misuc::ui {

VerticalMeter::VerticalMeter(QWidget* parent) : QWidget(parent) {}

void VerticalMeter::setLevels(float peakL, float rmsL, float peakR, float rmsR,
                              bool clipped) {
  peakL_ = peakL;
  rmsL_ = rmsL;
  peakR_ = peakR;
  rmsR_ = rmsR;
  clipped_ = clipped;
  update();
}

void VerticalMeter::drawBar(QPainter& p, const QRectF& bar, float level,
                            float& hold, float rms) {
  // Meters read dB from full scale; 0 dBFS sits at the top.
  auto dbOf = [](float l) {
    return l <= 0.f ? -60.f : 12.f * std::log10(l);
  };
  const float db = std::clamp(dbOf(level), -60.f, 0.f);
  const float fill = (60.f + db) / 60.f; // 0..1 from bottom

  // Track.
  p.fillRect(bar, theme::kMeterTrack);

  // Fill: green -> amber -> red top band.
  const float y0 = bar.bottom();       // bottom pixel baseline
  const float y1 = bar.top();          // full-scale pixel top
  const float fy = y1 + (1.f - fill) * (bar.height());
  QLinearGradient g(0, y1, 0, y0);
  g.setColorAt(0.0, theme::kDanger);
  g.setColorAt(0.35, QColor(0xff, 0xc9, 0x4a));
  g.setColorAt(1.0, theme::kOk);
  const QRectF fillRect(bar.left(), fy, bar.width(), std::max(1.f, y0 - fy));
  p.fillRect(fillRect, g);

  // RMS marker (thin line).
  const float rmsDb = std::clamp(dbOf(rms), -60.f, 0.f);
  const float ry = y1 + (1.f - (60.f + rmsDb) / 60.f) * bar.height();
  p.fillRect(QRectF(bar.left(), ry - 1.f, bar.width(), 1.5f),
             theme::kText);

  // Peak-hold cap.
  if (hold < level)
    hold = level;
  else
    hold *= 0.9992f; // slow release
  const float hdb = std::clamp(dbOf(hold), -60.f, 0.f);
  const float hy = y1 + (1.f - (60.f + hdb) / 60.f) * bar.height();
  p.fillRect(QRectF(bar.left(), hy - 1.f, bar.width(), 2.f),
             theme::kTextDim);
}

void VerticalMeter::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, false);

  const float w = width();
  const float gap = 2.f;
  const float barW = std::max(4.f, (w - gap) / 2.f);
  const float top = 2.f, bottom = height() - 4.f;
  const QRectF lBar(0.f, top, barW, bottom - top);
  const QRectF rBar(barW + gap, top, barW, bottom - top);

  drawBar(p, lBar, peakL_, holdL_, rmsL_);
  drawBar(p, rBar, peakR_, holdR_, rmsR_);

  // Clip LED.
  p.setPen(Qt::NoPen);
  p.setBrush(clipped_ ? theme::kDanger : theme::kMeterDim);
  const int led = std::max(4, width() / 4);
  p.drawEllipse(QRectF((w - led) / 2.f, 0.f, static_cast<float>(led),
                       static_cast<float>(led)));
}

} // namespace misuc::ui