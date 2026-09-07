#include "Knob.h"

#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "../Theme.h"

namespace misuc::ui {

Knob::Knob(QWidget* parent) : QWidget(parent) {
  setCursor(Qt::PointingHandCursor);
  setMouseTracking(true);
  setToolTip(QStringLiteral("Pan"));
}

void Knob::setRange(int min, int max) {
  min_ = min;
  max_ = max;
  setValue(value_);
}

void Knob::setValue(int v) {
  v = std::clamp(v, min_, max_);
  if (v == value_)
    return;
  value_ = v;
  update();
  emit valueChanged(v);
}

void Knob::setTitle(const QString& title) {
  title_ = title;
  update();
}

int Knob::fromPos(int y) const {
  // Vertical drag maps a ~120px travel across the whole range.
  const double span = std::max(1, max_ - min_);
  const double pos = static_cast<double>(max_ + min_) / 2.0 +
                     (static_cast<double>(lastY_ - y) / 120.0) * span;
  return static_cast<int>(std::lround(pos));
}

void Knob::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
  const QPointF c = r.center();
  const float rad = r.width() / 2.f;

  // Body.
  p.setBrush(theme::kPanelSunken);
  p.setPen(QPen(theme::kBorder, 1));
  p.drawEllipse(c, rad, rad);

  // Arc from -135..+135 degrees showing the range.
  const double frac = (value_ - min_) / static_cast<double>(max_ - min_);
  const double a0 = 135.0; // degrees swept from bottom-left
  const double sweep = -270.0 * frac;
  p.setPen(QPen(theme::kBorderBright, 2));
  QRectF arcBox(c.x() - rad * 0.82f, c.y() - rad * 0.82f, rad * 1.64f,
                rad * 1.64f);
  p.drawArc(arcBox, static_cast<int>(a0 * 16), static_cast<int>(sweep * 16));

  // Pointer line.
  const double ang = (a0 + sweep) * M_PI / 180.0;
  const QPointF tip(c.x() + std::cos(ang) * rad * 0.55f,
                    c.y() - std::sin(ang) * rad * 0.55f);
  p.setBrush(theme::kAccent);
  p.setPen(Qt::NoPen);
  p.drawLine(c, tip);
  p.setBrush(theme::kAccent);
  p.drawEllipse(tip, 2.4, 2.4);

  // Title underneath.
  if (!title_.isEmpty()) {
    p.setPen(theme::kTextDim);
    p.setFont(theme::monoFont(7));
    const QFontMetricsF fm(p.font());
    const QRectF tr = fm.boundingRect(title_);
    p.drawText(QRectF(0, height() - 14, width(), 14), Qt::AlignHCenter | Qt::AlignTop,
               title_);
  }
}

void Knob::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    dragging_ = true;
    lastY_ = event->pos().y();
    return;
  }
  QWidget::mousePressEvent(event);
}

void Knob::mouseMoveEvent(QMouseEvent* event) {
  if (dragging_) {
    const int delta = lastY_ - event->pos().y();
    setValue(value_ + delta);
    lastY_ = event->pos().y();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void Knob::mouseReleaseEvent(QMouseEvent* event) {
  dragging_ = false;
  QWidget::mouseReleaseEvent(event);
}

void Knob::mouseDoubleClickEvent(QMouseEvent*) {
  setValue(center_);
}

void Knob::wheelEvent(QWheelEvent* event) {
  const int step = std::max(1, (max_ - min_) / 100);
  setValue(value_ + (event->angleDelta().y() > 0 ? step : -step));
}

} // namespace misuc::ui