#include "Icons.h"

#include <cmath>

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

namespace misuc::ui::icons {

namespace {

QIcon render(const QColor& color, void (*draw)(QPainter&, const QRectF&)) {
  constexpr int kSize = 22;
  QPixmap pm(kSize, kSize);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.translate(0.5, 0.5);
  draw(p, QRectF(2, 2, kSize - 4, kSize - 4));
  p.end();
  return QIcon(pm);
}

void drawMetronome(QPainter& p, const QRectF& r) {
  QPen pen(p.pen());
  pen.setColor(p.pen().color());
  pen.setWidthF(1.7);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  // Tail housing: two converging sides.
  const double cx = r.center().x();
  p.drawLine(QPointF(cx, r.top()), QPointF(r.left() + 1.5, r.bottom() - 2));
  p.drawLine(QPointF(cx, r.top()), QPointF(r.right() - 1.5, r.bottom() - 2));
  // Base bar.
  p.drawLine(QPointF(r.left(), r.bottom() - 2), QPointF(r.right(), r.bottom() - 2));
  // Pendulum.
  p.drawLine(QPointF(cx, r.top() + 4), QPointF(r.right() - 3, r.bottom() - 7));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(r.right() - 3, r.bottom() - 7), 2.4, 2.4);
}

void drawLoop(QPainter& p, const QRectF& r) {
  QPen pen(p.pen());
  pen.setWidthF(1.7);
  pen.setCapStyle(Qt::RoundCap);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double rad = (qMin(r.width(), r.height()) / 2.0) * 0.72;
  // Left arc (top -> bottom) with downward arrowhead.
  p.drawArc(QRectF(cx - rad, cy - rad, 2 * rad, 2 * rad), 90 * 16, 130 * 16);
  QPointF tip(cx - rad + 4, cy + 5);
  QPainterPath arrow;
  arrow.moveTo(tip);
  arrow.lineTo(tip + QPointF(-4.5, -4.5));
  arrow.moveTo(tip);
  arrow.lineTo(tip + QPointF(4.5, -4.5));
  p.drawPath(arrow);
  // Right arc (bottom -> top) with upward arrowhead.
  p.drawArc(QRectF(cx - rad, cy - rad, 2 * rad, 2 * rad), -70 * 16, 130 * 16);
  QPointF tip2(cx + rad - 4, cy - 5);
  QPainterPath arrow2;
  arrow2.moveTo(tip2);
  arrow2.lineTo(tip2 + QPointF(-4.5, 4.5));
  arrow2.moveTo(tip2);
  arrow2.lineTo(tip2 + QPointF(4.5, 4.5));
  p.drawPath(arrow2);
}

void drawCountIn(QPainter& p, const QRectF& r) {
  const double cx = r.center().x();
  const double bottom = r.bottom();
  QPen pen(p.pen());
  pen.setWidthF(2.2);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  for (int i = 0; i < 3; ++i) {
    const double x = cx + (i - 1) * 6.5;
    const double h = 4.0 + i * 3.5;
    p.drawLine(QPointF(x, bottom - h), QPointF(x, bottom));
  }
}

void drawSettings(QPainter& p, const QRectF& r) {
  QPen pen(p.pen());
  pen.setWidthF(1.6);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double outer = qMin(r.width(), r.height()) / 2.0 - 1.0;
  const double inner = outer * 0.55;
  for (int i = 0; i < 8; ++i) {
    const double a = i * (M_PI / 4.0);
    QPointF d(std::cos(a), std::sin(a));
    p.drawLine(QPointF(cx + d.x() * inner, cy + d.y() * inner),
               QPointF(cx + d.x() * outer, cy + d.y() * outer));
  }
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QPointF(cx, cy), inner, inner);
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(cx, cy), inner * 0.42, inner * 0.42);
}

void drawAudio(QPainter& p, const QRectF& r) {
  const double top = r.top() + 3;
  const double bottom = r.bottom() - 3;
  QPainterPath body;
  body.moveTo(r.left(), top + 7);
  body.lineTo(r.left() + 6, top + 7);
  body.lineTo(r.left() + 11, top);
  body.lineTo(r.left() + 11, bottom);
  body.lineTo(r.left() + 6, bottom - 7);
  body.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  p.drawPath(body);
  QPen pen(p.pen().color());
  pen.setWidthF(1.7);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(r.left() + 9, top - 2, 8, r.height() - 2), -58 * 16, 116 * 16);
  p.drawArc(QRectF(r.left() + 12, top - 5, 11, r.height() + 4), -58 * 16, 116 * 16);
}

void drawQuestion(QPainter& p, const QRectF& r) {
  p.setBrush(Qt::NoBrush);
  QPen pen(p.pen());
  pen.setWidthF(1.8);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  const double cx = r.center().x();
  p.drawArc(QRectF(cx - 6, r.top(), 12, 13), 180 * 16, 140 * 16);
  p.drawLine(QPointF(cx + 3.5, r.top() + 12), QPointF(cx - 1.5, r.top() + 15));
  p.setPen(Qt::NoPen);
  p.setBrush(pen.color());
  p.drawEllipse(QPointF(cx - 1.5, r.bottom() - 1), 1.8, 1.8);
}

} // namespace

QIcon metronome(const QColor& color) { return render(color, drawMetronome); }
QIcon loop(const QColor& color) { return render(color, drawLoop); }
QIcon countIn(const QColor& color) { return render(color, drawCountIn); }
QIcon settings(const QColor& color) { return render(color, drawSettings); }
QIcon audio(const QColor& color) { return render(color, drawAudio); }
QIcon question(const QColor& color) { return render(color, drawQuestion); }

} // namespace misuc::ui::icons