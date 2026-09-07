#include "Fader.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "../Theme.h"

namespace misuc::ui {

Fader::Fader(QWidget* parent) : QWidget(parent) {
  setCursor(Qt::PointingHandCursor);
  setMouseTracking(true);
  setToolTip(QStringLiteral("Gain (dB). Double-click to reset to 0"));
}

void Fader::setValueDb(float db) {
  db = std::clamp(db, kMinDb, kMaxDb);
  if (std::fabs(db - valueDb_) < 0.01f)
    return;
  valueDb_ = db;
  update();
  emit valueChanged(db);
}

float Fader::fromPos(int y) const {
  const float pad = 10.f;
  const float usable = std::max(1.f, static_cast<float>(height()) - 2 * pad);
  const float t = std::clamp((static_cast<float>(y) - pad) / usable, 0.f, 1.f);
  // Invert so the top of the fader travel is the loudest (+12 dB).
  return kMaxDb - t * (kMaxDb - kMinDb);
}

void Fader::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const float cx = width() / 2.f;
  const float pad = 10.f;
  const float y0 = pad, y1 = height() - pad;

  // Groove (thin center track draw a slot).
  const float grooveW = 6.f;
  const float gx = cx - grooveW / 2.f;
  p.setRenderHint(QPainter::Antialiasing, false);
  p.fillRect(QRectF(gx, y0, grooveW, y1 - y0), theme::kPanelSunken);
  p.setPen(QPen(theme::kBorder, 1));
  p.drawRect(QRectF(gx, y0, grooveW, y1 - y0));

  // Filled portion below the value grows as the fader rises.
  const float t = (valueDb_ - kMinDb) / (kMaxDb - kMinDb);
  const float fy = y1 - t * (y1 - y0);
  QLinearGradient fill(0, y0, 0, y1);
  fill.setColorAt(0.f, theme::kAccent);
  fill.setColorAt(1.f, theme::kAccentSoft);
  if (fy < y1) {
    p.fillRect(QRectF(gx + 1, fy, grooveW - 2, y1 - fy), fill);
  }

  // Cap / handle.
  p.setRenderHint(QPainter::Antialiasing, true);
  const float capW = 34.f, capH = 16.f;
  const float capY = std::clamp(fy - capH / 2.f, y0, y1 - capH);
  p.setBrush(theme::kPanelRaised);
  p.setPen(QPen(theme::kBorderBright, 1));
  p.drawRoundedRect(QRectF(cx - capW / 2.f, capY, capW, capH), 4, 4);
  // Centre hairline on the cap.
  p.setPen(QPen(theme::kAccent, 2));
  p.drawLine(QPointF(cx - capW / 2.f + 6.f, capY + capH / 2.f),
             QPointF(cx + capW / 2.f - 6.f, capY + capH / 2.f));
}

void Fader::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    dragging_ = true;
    setValueDb(fromPos(event->pos().y()));
    return;
  }
  QWidget::mousePressEvent(event);
}

void Fader::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) {
    setValueDb(fromPos(event->pos().y()));
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void Fader::mouseReleaseEvent(QMouseEvent* event) {
  dragging_ = false;
  QWidget::mouseReleaseEvent(event);
}

void Fader::mouseDoubleClickEvent(QMouseEvent*) {
  setValueDb(0.f);
}

void Fader::wheelEvent(QWheelEvent* event) {
  setValueDb(valueDb_ + (event->angleDelta().y() > 0 ? 1.f : -1.f));
}

} // namespace misuc::ui