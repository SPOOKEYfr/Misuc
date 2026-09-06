#include "MeterWidget.h"

#include <QPainter>
#include <QtMath>

#include "Theme.h"

namespace misuc::ui {

namespace {
// dB scale mapping for the meter: [-60, 0] dB -> [0,1] fill.
float fillFromDb(float db) {
  const float x = qBound(-60.0f, db, 0.0f);
  return (x + 60.0f) / 60.0f;
}
float dbFromGain(float g) {
  if (g < 1e-5f)
    return -120.0f;
  return 20.0f * static_cast<float>(qLn(qreal(g)) / qLn(10.0));
}
} // namespace

MeterWidget::MeterWidget(QWidget* parent) : QWidget(parent) {
  setMinimumSize(48, 120);
}

QSize MeterWidget::sizeHint() const { return QSize(64, 160); }

void MeterWidget::setLevels(float peakL, float rmsL, float peakR, float rmsR,
                            bool clipped) {
  peakL_ = qMax(0.f, peakL);
  rmsL_ = qMax(0.f, rmsL);
  peakR_ = qMax(0.f, peakR);
  rmsR_ = qMax(0.f, rmsR);
  clipped_ = clipped;

  holdL_ = qMax(holdL_ - 0.002f, peakL_);
  holdR_ = qMax(holdR_ - 0.002f, peakR_);
  update();
}

void MeterWidget::drawChannel(QPainter& p, const QRectF& bar, float peak,
                              float rms, float& hold) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing, false);

  const double radius = 3.0;
  p.setPen(Qt::NoPen);
  p.setBrush(theme::kMeterTrack);
  p.drawRoundedRect(bar, radius, radius);

  // RMS fill (bottom-up).
  const float rmsF = fillFromDb(dbFromGain(rms));
  if (rmsF > 0.001f) {
    const double h = bar.height() * rmsF;
    QRectF fill(bar.x() + 1, bar.bottom() - h + 1, bar.width() - 2, h - 1);
    if (fill.height() > 1.0) {
      QColor c = theme::kAccent;
      if (rmsF > 0.75f)
        c = theme::kDanger;
      p.setBrush(c);
      p.drawRoundedRect(fill, radius, radius);
    }
  }

  // Peak cap + hold marker.
  const float peakF = fillFromDb(dbFromGain(peak));
  const double capY = bar.bottom() - bar.height() * peakF;
  QColor cap = theme::kOk;
  if (peakF > 0.75f)
    cap = theme::kDanger;
  p.setBrush(cap);
  p.drawRect(QRectF(bar.x(), capY - 1.5, bar.width(), 3.0));

  const float holdF = fillFromDb(dbFromGain(hold));
  const double holdY = bar.bottom() - bar.height() * holdF;
  p.setBrush(QColor(180, 190, 205));
  p.drawRect(QRectF(bar.x(), holdY - 1.0, bar.width(), 1.5));
  p.restore();
}

void MeterWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const double spacing = 6.0;
  const double w = (width() - spacing) / 2.0;
  const double h = height() - 18.0;
  QRectF lR(0, 0, w, h);
  QRectF rR(w + spacing, 0, w, h);

  // dB labels.
  p.setPen(theme::kTextFaint);
  const QFont tiny = theme::uiFont(7, QFont::DemiBold);
  p.setFont(tiny);
  for (int db = 0; db >= -60; db -= 20) {
    const double y = h * fillFromDb(static_cast<float>(db));
    p.drawText(QRectF(0, y - 6, w, 10), Qt::AlignRight,
               QString::number(db));
  }

  drawChannel(p, lR.adjusted(0, 0, -4, 0), peakL_, rmsL_, holdL_);
  drawChannel(p, rR.adjusted(4, 0, 0, 0), peakR_, rmsR_, holdR_);

  // Clip LED.
  const QRectF led(lR.center().x() - w / 4.0, h + 6, w * 1.5, 8);
  p.setBrush(clipped_ ? theme::kDanger : theme::kPanelRaised);
  p.setPen(theme::kBorder);
  p.drawRoundedRect(led, 3, 3);
  p.setPen(theme::kTextFaint);
  const QFont f = theme::uiFont(7, QFont::DemiBold);
  p.setFont(f);
  p.drawText(led, Qt::AlignCenter, QStringLiteral("CLIP"));
}

} // namespace misuc::ui