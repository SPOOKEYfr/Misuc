#include "Icons.h"

#include <cmath>

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

namespace misuc::ui::icons {

namespace {

struct Pen {
  QPen p;
  Pen(const QColor& color, double w) {
    p.setColor(color);
    p.setWidthF(w);
    p.setCapStyle(Qt::RoundCap);
    p.setJoinStyle(Qt::RoundJoin);
  }
  operator QPen() const { return p; }
};

// Taper-safe line drawing that snaps to the sharp half-pixel grid.
void line(QPainter& p, QPointF a, QPointF b) { p.drawLine(a, b); }

void drawPlay(QPainter& p, const QRectF& r) {
  const double cx = r.center().x();
  const double cy = r.center().y();
  QPainterPath tri;
  tri.moveTo(cx + r.width() * 0.28, cy);
  tri.lineTo(cx - r.width() * 0.2, cy + r.height() * 0.34);
  tri.lineTo(cx - r.width() * 0.2, cy - r.height() * 0.34);
  tri.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  p.drawPath(tri);
}

void drawStop(QPainter& p, const QRectF& r) {
  const double s = qMin(r.width(), r.height()) * 0.5;
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  p.drawRect(QRectF(r.center().x() - s / 2, r.center().y() - s / 2, s, s));
}

void drawRecord(QPainter& p, const QRectF& r) {
  const double rad = qMin(r.width(), r.height()) * 0.3;
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  p.drawEllipse(r.center(), rad, rad);
}

void drawMetronome(QPainter& p, const QRectF& r) {
  const double cx = r.center().x();
  p.setPen(Pen(p.pen().color(), 1.7));
  p.setBrush(Qt::NoBrush);
  line(p, QPointF(cx, r.top()), QPointF(r.left() + 1.5, r.bottom() - 2));
  line(p, QPointF(cx, r.top()), QPointF(r.right() - 1.5, r.bottom() - 2));
  line(p, QPointF(r.left(), r.bottom() - 2), QPointF(r.right(), r.bottom() - 2));
  line(p, QPointF(cx, r.top() + 4), QPointF(r.right() - 3, r.bottom() - 7));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(r.right() - 3, r.bottom() - 7), 2.2, 2.2);
}

void drawLoop(QPainter& p, const QRectF& r) {
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double rad = (qMin(r.width(), r.height()) / 2.0) * 0.72;
  p.setPen(Pen(p.pen().color(), 1.7));
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(cx - rad, cy - rad, 2 * rad, 2 * rad), 90 * 16, 130 * 16);
  QPointF tip(cx - rad + 4, cy + 5);
  QPainterPath arrow;
  arrow.moveTo(tip);
  arrow.lineTo(tip + QPointF(-4.5, -4.5));
  arrow.moveTo(tip);
  arrow.lineTo(tip + QPointF(4.5, -4.5));
  p.drawPath(arrow);
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
  p.setPen(Pen(p.pen().color(), 2.0));
  p.setBrush(Qt::NoBrush);
  for (int i = 0; i < 3; ++i) {
    const double x = cx + (i - 1) * 6.0;
    const double h = 4.0 + i * 3.0;
    line(p, QPointF(x, bottom - h), QPointF(x, bottom));
  }
}

void drawSnap(QPainter& p, const QRectF& r) {
  const double cx = r.center().x();
  const double cy = r.center().y();
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Magnet arc with two snapped verticals.
  p.drawArc(QRectF(cx - 5, cy - 2, 10, 8), 0, 180 * 16);
  line(p, QPointF(cx - 1, cy - 1), QPointF(cx - 1, r.top() + 2));
  line(p, QPointF(cx + 2, cy - 1), QPointF(cx + 2, r.top() + 2));
}

void drawSongMode(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Song: an arrangement of blocks.
  const double w = r.width() / 6.0;
  const double h = r.height() / 3.0;
  for (int i = 0; i < 6; ++i)
    p.drawRect(QRectF(r.left() + i * w, r.top() + (i % 3) * h, w - 1, h - 1));
}

void drawPatternMode(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Pattern: a single repeating 4-cell strip.
  const double w = r.width() / 4.0;
  const double h = r.height() * 0.55;
  for (int i = 0; i < 4; ++i)
    p.drawRect(QRectF(r.left() + i * w, r.top() + (r.height() - h) / 2, w - 1, h));
}

void drawPlaylist(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Track lanes with the violet playhead line.
  const double laneH = r.height() / 3.0;
  for (int i = 0; i < 3; ++i) {
    const double y = r.top() + i * laneH;
    const double x = r.left() + 4 + (i % 2) * 3.5;
    p.drawRect(QRectF(x, y + 1, r.width() - x - 6, laneH - 3));
  }
  p.setPen(Pen(p.pen().color(), 1.7));
  line(p, QPointF(r.right() - 3, r.top()), QPointF(r.right() - 3, r.bottom()));
}

void drawPianoRoll(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.4));
  p.setBrush(Qt::NoBrush);
  // Horizontal grate with a long note bar on top.
  for (int i = 0; i < 5; ++i) {
    const double y = r.top() + 4 + i * (r.height() - 8) / 4.0;
    line(p, QPointF(r.left() + 4, y), QPointF(r.right() - 4, y));
  }
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawRect(QRectF(r.left() + 4, r.top() + 1, r.width() * 0.4, 4));
}

void drawChannelRack(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // A column of sample channels with a speaker tripod.
  const double cw = r.width() * 0.45;
  for (int i = 0; i < 4; ++i) {
    const double y = r.top() + 4 + i * (r.height() - 8) / 4.0;
    p.drawRect(QRectF(r.left() + 2, y, cw - 3, 3));
    const double x = r.left() + cw + 3;
    line(p, QPointF(x, r.top()), QPointF(x, r.bottom()));
    line(p, QPointF(x - 1.5, r.top() + 2), QPointF(x + 1.5, r.top() + 2));
  }
}

void drawMixer(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Three channel strips with vertical faders.
  const double cw = r.width() / 3.0;
  for (int i = 0; i < 3; ++i) {
    const double x = r.left() + i * cw + cw * 0.18;
    const double w = cw * 0.64;
    p.drawRoundedRect(QRectF(x, r.top() + 2, w, r.height() - 6), 1.5, 1.5);
    const double fy = r.top() + 5 + (r.height() - 16) * (i % 3 == 0 ? 0.15 : 0.75);
    line(p, QPointF(x + w / 2, r.top() + 3), QPointF(x + w / 2, r.bottom() - 3));
    p.drawRect(QRectF(x + w * 0.2, fy, w * 0.6, 4));
  }
}

void drawBrowser(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.4));
  p.setBrush(Qt::NoBrush);
  line(p, QPointF(r.left() + 1, r.top()), QPointF(r.left() + 1, r.bottom()));
  line(p, QPointF(r.left() + 1, r.top()), QPointF(r.left() + 6, r.top() + 2));
  line(p, QPointF(r.left() + 6, r.top() + 2), QPointF(r.left() + 3, r.top() + 6));
  // File rows.
  for (int i = 0; i < 3; ++i) {
    const double y = r.top() + 7 + i * 6;
    line(p, QPointF(r.left() + 5, y), QPointF(r.left() + 10, y));
    line(p, QPointF(r.left() + 12, y), QPointF(r.right() - 1, y));
  }
  line(p, QPointF(r.left() + 5, r.top() + 25), QPointF(r.right() - 1, r.top() + 25));
}

void drawInspector(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  line(p, QPointF(r.right() - 1, r.top()), QPointF(r.right() - 1, r.bottom()));
  line(p, QPointF(r.left() + 1, r.top()), QPointF(r.left() + 1, r.bottom() - 3));
  line(p, QPointF(r.left() + 1, r.bottom() - 3), QPointF(r.right() - 1, r.bottom() - 3));
  // Property rows.
  for (int i = 0; i < 3; ++i) {
    const double y = r.top() + 4 + i * 7;
    line(p, QPointF(r.left() + 6, y + 1), QPointF(r.left() + 11, y + 1));
    line(p, QPointF(r.left() + 13, y), QPointF(r.right() - 3, y));
    line(p, QPointF(r.left() + 13, y + 3), QPointF(r.right() - 3, y + 3));
  }
}

void drawPlugins(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  const double cx = r.center().x();
  const double cy = r.center().y();
  // A puzzle-ish block with a jack plug.
  p.drawRoundedRect(QRectF(cx - 6, cy - 6, 12, 12), 2, 2);
  line(p, QPointF(cx - 1, cy + 6), QPointF(cx - 1, r.bottom() - 2));
  line(p, QPointF(cx + 1, cy + 6), QPointF(cx + 1, r.bottom() - 2));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(cx, r.bottom() - 2), 1.6, 1.6);
}

void drawSelect(QPainter& p, const QRectF& r) {
  const double cx = r.left() + 6;
  const double cy = r.top() + 5;
  QPainterPath cur;
  cur.moveTo(cx, cy);
  cur.lineTo(cx + 1, r.bottom() - 4);
  cur.lineTo(cx + 6, r.bottom() - 10);
  cur.lineTo(cx + 10, r.bottom() - 3);
  cur.lineTo(cx + 4, r.bottom() - 9);
  cur.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  p.drawPath(cur);
}

void drawDraw(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.7));
  p.setBrush(Qt::NoBrush);
  // Pencil at 45 degrees.
  line(p, QPointF(r.left() + 3, r.bottom() - 3), QPointF(r.right() - 3, r.top() + 3));
  line(p, QPointF(r.right() - 7, r.top() - 1), QPointF(r.right() - 4, r.top() - 4));
}

void drawPaint(QPainter& p, const QRectF& r) {
  // Paint roller with a round head.
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  p.drawRect(QRectF(r.left() + 3, r.top() + 5, r.width() - 8, 5));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(r.left() + 5, r.bottom() - 3), 2, 2);
}

void drawErase(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Eraser tilted: a slanted rounded block.
  line(p, QPointF(r.left() + 2, r.bottom() - 3), QPointF(r.left() + 5, r.top() + 3));
  line(p, QPointF(r.left() + 4, r.top() + 1), QPointF(r.right() - 5, r.top() + 4));
  line(p, QPointF(r.right() - 5, r.top() + 4), QPointF(r.right() - 3, r.bottom() - 5));
  line(p, QPointF(r.right() - 3, r.bottom() - 5), QPointF(r.left() + 2, r.bottom() - 3));
}

void drawSlice(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Scissors open.
  const double cx = r.center().x();
  line(p, QPointF(cx - 4, r.bottom() - 2), QPointF(r.left() + 2, r.top() + 2));
  line(p, QPointF(cx - 4, r.bottom() - 2), QPointF(r.right() - 2, r.top() + 2));
  line(p, QPointF(cx - 4, r.bottom() - 2), QPointF(r.left() + 5, r.top() + 6));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(r.right() - 2, r.top() + 2), 2, 2);
  p.drawEllipse(QPointF(r.left() + 2, r.top() + 2), 2, 2);
}

void drawZoomIn(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.7));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QRectF(r.left() + 2, r.top() + 2, r.width() * 0.5, r.height() * 0.5));
  line(p, QPointF(r.left() + 2 + r.width() * 0.42, r.top() + 2 + r.height() * 0.42),
       QPointF(r.right() - 2, r.bottom() - 2));
  const double cx = r.left() + 2 + r.width() * 0.25;
  const double cy = r.top() + 2 + r.height() * 0.25;
  line(p, QPointF(cx, cy - 3), QPointF(cx, cy + 3));
  line(p, QPointF(cx - 3, cy), QPointF(cx + 3, cy));
}

void drawMute(QPainter& p, const QRectF& r) {
  // Filled speaker with a crossed-out diaphragm.
  const double cx = r.left() + 6;
  QPainterPath body;
  body.moveTo(cx - 3, r.top() + 5);
  body.lineTo(cx + 1, r.top() + 5);
  body.lineTo(cx + 5, r.top() + 2);
  body.lineTo(cx + 5, r.bottom() - 2);
  body.lineTo(cx + 1, r.bottom() - 5);
  body.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  p.drawPath(body);
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(cx - 2, r.top() + 3, 8, r.height() - 6), -80 * 16, 160 * 16);
  line(p, QPointF(r.right() - 5, r.top() + 3), QPointF(r.right() - 1, r.bottom() - 4));
  line(p, QPointF(r.right() - 1, r.top() + 3), QPointF(r.right() - 5, r.bottom() - 4));
}

void drawSolo(QPainter& p, const QRectF& r) {
  // A single lit bulb with a shine.
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(r.center().x(), r.top() + 7), 3.5, 3.5);
  p.drawRect(QRectF(r.center().x() - 1.5, r.top() + 10, 3, 3));
  p.setPen(Pen(p.pen().color(), 1));
  p.setBrush(Qt::NoBrush);
  line(p, QPointF(r.center().x(), r.top() + 13), QPointF(r.center().x(), r.bottom() - 2));
}

void drawArm(QPainter& p, const QRectF& r) {
  // A record arm dot on a small stand.
  const double cx = r.center().x();
  const double cy = r.top() + 6;
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QPointF(cx, cy), 3.2, 3.2);
  line(p, QPointF(cx, cy + 3), QPointF(cx, r.bottom() - 2));
  line(p, QPointF(cx - 4, r.bottom() - 2), QPointF(cx + 4, r.bottom() - 2));
}

void drawVisibilityOn(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.4));
  p.setBrush(Qt::NoBrush);
  const double cy = r.center().y();
  p.drawEllipse(QPointF(r.left() + 4, cy), 1.8, 1.8);
  p.drawArc(QRectF(r.left() - 6, cy - 5, 16, 10), -60 * 16, 120 * 16);
  p.drawArc(QRectF(r.right() - 10, cy - 5, 16, 10), 120 * 16, 120 * 16);
}

void drawVisibilityOff(QPainter& p, const QRectF& r) {
  drawVisibilityOn(p, r);
  p.setPen(Pen(p.pen().color(), 1.6));
  line(p, QPointF(r.left() + 1, r.top() + 1), QPointF(r.right() - 1, r.bottom() - 1));
}

void drawOptions(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.9));
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double w = r.width() * 0.5;
  const double h = r.height() * 0.5;
  line(p, QPointF(cx - w, cy - h), QPointF(cx + w, cy - h));
  line(p, QPointF(cx - w, cy), QPointF(cx + w, cy));
  line(p, QPointF(cx - w, cy + h), QPointF(cx + w, cy + h));
}

void drawInstruments(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Piano key slice.
  for (int i = 0; i < 4; ++i) {
    const double x = r.left() + 2 + i * 4;
    line(p, QPointF(x, r.top() + 2), QPointF(x, r.bottom() - 2));
  }
  line(p, QPointF(r.left() + 2, r.top() + 2), QPointF(r.right() - 2, r.top() + 2));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawRect(QRectF(r.left() + 8, r.bottom() - 5, 2, 3));
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
  pen.setWidthF(1.6);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(r.left() + 9, top - 2, 8, r.height() - 2), -58 * 16, 116 * 16);
  p.drawArc(QRectF(r.left() + 12, top - 5, 11, r.height() + 4), -58 * 16, 116 * 16);
}

void drawMidi(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Sheet music: staff + note dots.
  for (int i = 0; i < 4; ++i) {
    const double y = r.top() + 4 + i * 5;
    line(p, QPointF(r.left() + 2, y), QPointF(r.right() - 2, y));
  }
  const double cx = r.left() + 9;
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(cx, r.top() + 10), 2.4, 1.8);
  line(p, QPointF(cx + 2.0, r.top() + 10), QPointF(cx + 2.0, r.top() + 1));
}

void drawSamples(QPainter& p, const QRectF& r) {
  // Stacked waveform slices.
  for (int i = 0; i < 3; ++i) {
    const double y = r.top() + 3 + i * 7;
    const double h = 5;
    QPainterPath w;
    w.moveTo(r.left() + 3, y + h / 2);
    w.cubicTo(r.left() + 6, y, r.left() + 8, y + h, r.left() + 11, y + h / 2);
    w.cubicTo(r.left() + 13, y, r.left() + 15, y + h, r.right() - 3, y + h / 2);
    p.setPen(Pen(p.pen().color(), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawPath(w);
  }
}

void drawEffects(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Slider panel: two horizontal sliders.
  const double cx = r.center().x();
  const double step = (r.height() - 10) / 2.0;
  for (int i = 0; i < 2; ++i) {
    const double y = r.top() + 5 + i * step;
    line(p, QPointF(r.left() + 2, y), QPointF(r.right() - 2, y));
    p.drawRect(QRectF(cx - 2.5, y - 2.5, 5, 5));
  }
}

void drawPatterns(QPainter& p, const QRectF& r) {
  drawPatternMode(p, r);
}

void drawAutomation(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // A rising then falling control line.
  const double top = r.top() + 2;
  const double bot = r.bottom() - 2;
  QPainterPath a;
  a.moveTo(r.left() + 2, bot - 3);
  a.quadTo(r.left() + r.width() * 0.4, top, r.center().x(), bot);
  a.quadTo(r.right() - r.width() * 0.3, top, r.right() - 2, bot - 2);
  p.drawPath(a);
}

void drawAllItems(QPainter& p, const QRectF& r) {
  // 2x2 grid of small squares.
  const double cw = r.width() / 2.0;
  const double ch = r.height() / 2.0;
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j)
      p.drawRect(QRectF(r.left() + i * cw + 1, r.top() + j * ch + 1, cw - 2, ch - 2));
}

void drawCurrentProject(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Document with a musical note.
  QPainterPath doc;
  doc.moveTo(r.left() + 3, r.top());
  doc.lineTo(r.right() - 5, r.top());
  doc.lineTo(r.right() - 3, r.top() + 2);
  doc.lineTo(r.right() - 3, r.bottom());
  doc.lineTo(r.left() + 3, r.bottom());
  doc.closeSubpath();
  p.drawPath(doc);
  line(p, QPointF(r.left() + 3, r.top()), QPointF(r.left() + 3, r.top() + 3));
  line(p, QPointF(r.left() + 3, r.top() + 3), QPointF(r.right() - 5, r.top()));
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(r.left() + 7, r.top() + 8), 2, 1.7);
  line(p, QPointF(r.left() + 9, r.top() + 8), QPointF(r.left() + 9, r.top() + 3));
}

void drawFavorites(QPainter& p, const QRectF& r) {
  // Star.
  const QPointF c = r.center();
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  QPainterPath star;
  const double R = r.width() * 0.42;
  const double rr = R * 0.45;
  for (int i = 0; i < 10; ++i) {
    const double rad = (i % 2 == 0) ? R : rr;
    const double a = -M_PI / 2 + i * M_PI / 5;
    const QPointF pt(c.x() + rad * std::cos(a), c.y() + rad * std::sin(a));
    if (i == 0)
      star.moveTo(pt);
    else
      star.lineTo(pt);
  }
  star.closeSubpath();
  p.drawPath(star);
}

void drawRecent(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  const QRectF ring(QRectF(r.left() + 2, r.top() + 2, r.width() - 4, r.height() - 4));
  p.drawArc(ring, 0, 360 * 16);
  // Clock hands.
  const double cx = r.center().x();
  const double cy = r.center().y();
  line(p, QPointF(cx, cy), QPointF(cx, cy - 4));
  line(p, QPointF(cx, cy), QPointF(cx + 3, cy + 2));
}

void drawSearch(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.7));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QRectF(r.left() + 2, r.top() + 2, r.width() * 0.5, r.height() * 0.5));
  line(p, QPointF(r.left() + 2 + r.width() * 0.4, r.top() + 2 + r.height() * 0.4),
       QPointF(r.right() - 2, r.bottom() - 2));
}

void drawAdd(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.7));
  const double cx = r.center().x();
  const double cy = r.center().y();
  line(p, QPointF(cx, r.top() + 4), QPointF(cx, r.bottom() - 4));
  line(p, QPointF(r.left() + 4, cy), QPointF(r.right() - 4, cy));
}

void drawRemove(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.7));
  line(p, QPointF(r.left() + 4, r.center().y()), QPointF(r.right() - 4, r.center().y()));
}

void drawDuplicate(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Two overlapping rounded rects.
  p.drawRoundedRect(QRectF(r.left() + 2, r.top() + 4, 9, 9), 1.5, 1.5);
  p.drawRoundedRect(QRectF(r.left() + 7, r.top() + 1, 9, 9), 1.5, 1.5);
}

void drawRename(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  // Text cursor over a bar.
  p.drawRect(QRectF(r.left() + 2, r.top() + 3, 10, 6));
  line(p, QPointF(r.right() - 2, r.bottom() - 3), QPointF(r.right() - 6, r.top() + 3));
}

void drawFolder(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  QPainterPath f;
  f.moveTo(r.left() + 1, r.top() + 3);
  f.lineTo(r.left() + 6, r.top() + 3);
  f.lineTo(r.left() + 8, r.top() + 6);
  f.lineTo(r.right() - 1, r.top() + 6);
  f.lineTo(r.right() - 1, r.bottom() - 2);
  f.lineTo(r.left() + 1, r.bottom() - 2);
  f.closeSubpath();
  p.drawPath(f);
}

void drawChevronRight(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.8));
  p.setBrush(Qt::NoBrush);
  line(p, QPointF(r.left() + 5, r.top() + 2), QPointF(r.right() - 3, r.center().y()));
  line(p, QPointF(r.left() + 5, r.bottom() - 2), QPointF(r.right() - 3, r.center().y()));
}

void drawChevronDown(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.8));
  p.setBrush(Qt::NoBrush);
  line(p, QPointF(r.left() + 2, r.top() + 5), QPointF(r.center().x(), r.bottom() - 3));
  line(p, QPointF(r.right() - 2, r.top() + 5), QPointF(r.center().x(), r.bottom() - 3));
}

void drawNewDoc(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  QPainterPath d;
  d.moveTo(r.left() + 3, r.top());
  d.lineTo(r.right() - 5, r.top());
  d.lineTo(r.right() - 3, r.bottom() - 2);
  d.lineTo(r.left() + 3, r.bottom() - 2);
  d.closeSubpath();
  p.drawPath(d);
  p.setBrush(p.pen().color());
  p.drawRect(QRectF(r.center().x() - 1.5, r.top() + 4, 3, 6));
  p.drawRect(QRectF(r.center().x() - 3.5, r.top() + 6, 7, 3));
}

void drawOpenDoc(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  QPainterPath d;
  d.moveTo(r.left() + 3, r.top() + 1);
  d.lineTo(r.right() - 5, r.top() + 1);
  d.lineTo(r.right() - 3, r.bottom() - 2);
  d.lineTo(r.left() + 3, r.bottom() - 2);
  d.closeSubpath();
  p.drawPath(d);
  line(p, QPointF(r.left() + 3, r.top() + 1), QPointF(r.left() + 3, r.top() + 5));
}

void drawSaveDoc(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  QPainterPath d;
  d.moveTo(r.left() + 3, r.top());
  d.lineTo(r.right() - 5, r.top());
  d.lineTo(r.right() - 3, r.bottom() - 2);
  d.lineTo(r.left() + 3, r.bottom() - 2);
  d.closeSubpath();
  p.drawPath(d);
  // Floppy lower body.
  p.drawRect(QRectF(r.center().x() - 3, r.bottom() - 7, 6, 5));
}

void drawExportDoc(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  QPainterPath d;
  d.moveTo(r.left() + 3, r.top() + 3);
  d.lineTo(r.right() - 5, r.top() + 3);
  d.lineTo(r.right() - 3, r.bottom() - 2);
  d.lineTo(r.left() + 3, r.bottom() - 2);
  d.closeSubpath();
  p.drawPath(d);
  p.setPen(Pen(p.pen().color(), 1.6));
  line(p, QPointF(r.center().x(), r.top() - 3), QPointF(r.center().x(), r.top() + 4));
  line(p, QPointF(r.left() + 4, r.top() - 3), QPointF(r.center().x(), r.bottom() + 2));
}

void drawUndo(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(r.left(), r.top() + 2, r.width() * 0.7, r.height() * 0.7), 0, 240 * 16);
  QPainterPath arr;
  const QPointF tip(r.left() + 1, r.bottom() - 6);
  arr.moveTo(tip);
  arr.lineTo(tip + QPointF(5, -2));
  arr.moveTo(tip);
  arr.lineTo(tip + QPointF(5, 2));
  p.drawPath(arr);
}

void drawRedo(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(r.right() - r.width() * 0.7, r.top() + 2, r.width() * 0.7,
                   r.height() * 0.7),
            0, -240 * 16);
  QPainterPath arr;
  const QPointF tip(r.right() - 1, r.bottom() - 6);
  arr.moveTo(tip);
  arr.lineTo(tip + QPointF(-5, -2));
  arr.moveTo(tip);
  arr.lineTo(tip + QPointF(-5, 2));
  p.drawPath(arr);
}

void drawClose(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.6));
  line(p, QPointF(r.left() + 3, r.top() + 3), QPointF(r.right() - 3, r.bottom() - 3));
  line(p, QPointF(r.left() + 3, r.bottom() - 3), QPointF(r.right() - 3, r.top() + 3));
}

void drawPin(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  // Push pin: head + pin line.
  const double cx = r.center().x();
  p.drawEllipse(QPointF(cx, r.top() + 4), 3.5, 3.5);
  line(p, QPointF(cx, r.top() + 7), QPointF(cx, r.bottom() - 2));
}

void drawFloat(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.5));
  p.setBrush(Qt::NoBrush);
  p.drawRect(QRectF(r.left() + 1, r.top() + 1, r.width() - 7, r.height() - 7));
  line(p, QPointF(r.left() + 4, r.bottom() - 1), QPointF(r.right() - 1, r.top() + 4));
}

void drawPreview(QPainter& p, const QRectF& r) {
  // Waveform play head (small monitor).
  p.setPen(Pen(p.pen().color(), 1.4));
  p.setBrush(Qt::NoBrush);
  for (int i = 0; i < 4; ++i) {
    const double x = r.left() + 2 + i * 4;
    line(p, QPointF(x, r.top() + 5), QPointF(x, r.bottom() - 5));
  }
  const double cx = r.center().x();
  const double cy = r.center().y();
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  QPainterPath tri;
  tri.moveTo(cx + 3, cy);
  tri.lineTo(cx - 12, cy - 4);
  tri.lineTo(cx - 12, cy + 4);
  tri.closeSubpath();
  p.drawPath(tri);
}

void drawQuestion(QPainter& p, const QRectF& r) {
  p.setBrush(Qt::NoBrush);
  QPen pen(p.pen());
  pen.setWidthF(1.7);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  const double cx = r.center().x();
  p.drawArc(QRectF(cx - 6, r.top(), 12, 13), 180 * 16, 140 * 16);
  line(p, QPointF(cx + 3.5, r.top() + 12), QPointF(cx - 1.5, r.top() + 15));
  p.setPen(Qt::NoPen);
  p.setBrush(pen.color());
  p.drawEllipse(QPointF(cx - 1.5, r.bottom() - 1), 1.8, 1.8);
}

void drawSettings(QPainter& p, const QRectF& r) {
  QPen pen(p.pen());
  pen.setWidthF(1.5);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double outer = qMin(r.width(), r.height()) / 2.0 - 1.0;
  const double inner = outer * 0.55;
  for (int i = 0; i < 8; ++i) {
    const double a = i * (M_PI / 4.0);
    QPointF d(std::cos(a), std::sin(a));
    line(p, QPointF(cx + d.x() * inner, cy + d.y() * inner),
         QPointF(cx + d.x() * outer, cy + d.y() * outer));
  }
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(QPointF(cx, cy), inner, inner);
  p.setBrush(p.pen().color());
  p.setPen(Qt::NoPen);
  p.drawEllipse(QPointF(cx, cy), inner * 0.42, inner * 0.42);
}

void drawWaveform(QPainter& p, const QRectF& r) {
  p.setPen(Pen(p.pen().color(), 1.4));
  p.setBrush(Qt::NoBrush);
  QPainterPath w;
  w.moveTo(r.left() + 2, r.center().y());
  for (int i = 0; i <= 8; ++i) {
    const double x = r.left() + 2 + i * (r.width() - 4) / 8.0;
    const double amp = (i % 2 == 0) ? (r.height() * 0.2) : (r.height() * 0.45);
    w.lineTo(x, i < 4 ? r.center().y() - amp : r.center().y() + amp);
  }
  p.drawPath(w);
}

void drawRefresh(QPainter& p, const QRectF& r) {
  // Circular arrow: open arc with a barbed head, clockwise.
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double R = r.width() * 0.32;
  QPen pen(p.pen().color());
  pen.setWidthF(1.5);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  const QRectF ring(cx - R, cy - R, 2 * R, 2 * R);
  // Arc open to the top-right; the barrel gap lets the arrowhead stand out.
  p.drawArc(ring, 120 * 16, 300 * 16);
  // Arrowhead at the top-right end of the arc.
  QPen headPen(p.pen().color());
  headPen.setWidthF(1.5);
  headPen.setCapStyle(Qt::RoundCap);
  p.setPen(headPen);
  const double rx = R * 0.707;
  p.drawLine(QPointF(cx + rx, cy - rx), QPointF(cx + rx + 3.5, cy - rx - 3.5));
  p.drawLine(QPointF(cx + rx, cy - rx), QPointF(cx + rx + 4.2, cy - rx + 1.4));
}

void drawApp(QPainter& p, const QRectF& r) {
  const double cx = r.center().x();
  const double cy = r.center().y();
  const double R = r.width() * 0.36;
  // Music note stem + head, flame-flavoured teardrop.
  p.setPen(Qt::NoPen);
  p.setBrush(p.pen().color());
  QPainterPath note;
  note.moveTo(cx + 1, r.top() + 1);
  note.lineTo(cx + 5.5, r.top() + 1);
  note.lineTo(cx + 5.5, r.bottom() - 7);
  note.lineTo(cx + 1, r.bottom() - 7);
  note.closeSubpath();
  p.drawPath(note);
  p.drawEllipse(QPointF(cx + 2, r.bottom() - 1), 3.4, 2.4);
  p.drawEllipse(QPointF(cx - 5, r.bottom() - 6), 3.4, 2.4);
  // Flame: a tapered arc on the note head.
  QPen pen(p.pen().color());
  pen.setWidthF(1.6);
  pen.setCapStyle(Qt::RoundCap);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawArc(QRectF(cx - R, cy - R, 2 * R, 2 * R), 200 * 16, 120 * 16);
}

using DrawFn = void (*)(QPainter&, const QRectF&);

DrawFn drawFn(IconId id) {
  switch (id) {
    case IconId::App: return drawApp;
    case IconId::Playlist: return drawPlaylist;
    case IconId::PianoRoll: return drawPianoRoll;
    case IconId::ChannelRack: return drawChannelRack;
    case IconId::Mixer: return drawMixer;
    case IconId::Browser: return drawBrowser;
    case IconId::Inspector: return drawInspector;
    case IconId::Plugins: return drawPlugins;
    case IconId::Play: return drawPlay;
    case IconId::Stop: return drawStop;
    case IconId::Record: return drawRecord;
    case IconId::Metronome: return drawMetronome;
    case IconId::CountIn: return drawCountIn;
    case IconId::Loop: return drawLoop;
    case IconId::Snap: return drawSnap;
    case IconId::SongMode: return drawSongMode;
    case IconId::PatternMode: return drawPatternMode;
    case IconId::Select: return drawSelect;
    case IconId::Draw: return drawDraw;
    case IconId::Paint: return drawPaint;
    case IconId::Erase: return drawErase;
    case IconId::Slice: return drawSlice;
    case IconId::ZoomIn: return drawZoomIn;
    case IconId::Mute: return drawMute;
    case IconId::Solo: return drawSolo;
    case IconId::Arm: return drawArm;
    case IconId::VisibilityOn: return drawVisibilityOn;
    case IconId::VisibilityOff: return drawVisibilityOff;
    case IconId::Options: return drawOptions;
    case IconId::Instruments: return drawInstruments;
    case IconId::Audio: return drawAudio;
    case IconId::Midi: return drawMidi;
    case IconId::Samples: return drawSamples;
    case IconId::Effects: return drawEffects;
    case IconId::Patterns: return drawPatterns;
    case IconId::Automation: return drawAutomation;
    case IconId::AllItems: return drawAllItems;
    case IconId::CurrentProject: return drawCurrentProject;
    case IconId::Favorites: return drawFavorites;
    case IconId::Recent: return drawRecent;
    case IconId::Search: return drawSearch;
    case IconId::Add: return drawAdd;
    case IconId::Remove: return drawRemove;
    case IconId::Duplicate: return drawDuplicate;
    case IconId::Rename: return drawRename;
    case IconId::Folder: return drawFolder;
    case IconId::ChevronRight: return drawChevronRight;
    case IconId::ChevronDown: return drawChevronDown;
    case IconId::NewDoc: return drawNewDoc;
    case IconId::OpenDoc: return drawOpenDoc;
    case IconId::SaveDoc: return drawSaveDoc;
    case IconId::ExportDoc: return drawExportDoc;
    case IconId::Undo: return drawUndo;
    case IconId::Redo: return drawRedo;
    case IconId::Close: return drawClose;
    case IconId::Pin: return drawPin;
    case IconId::Float: return drawFloat;
    case IconId::Preview: return drawPreview;
    case IconId::Question: return drawQuestion;
    case IconId::Settings: return drawSettings;
    case IconId::Waveform: return drawWaveform;
    case IconId::Refresh: return drawRefresh;
    default: return nullptr;
  }
}

} // namespace

QIcon make(IconId id, const QColor& color, int size) {
  DrawFn fn = drawFn(id);
  if (fn == nullptr)
    return QIcon();
  QPixmap pm(size, size);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.translate(0.5, 0.5);
  p.setPen(Pen(color, 1.4));
  p.setBrush(Qt::NoBrush);
  fn(p, QRectF(1.5, 1.5, size - 3, size - 3));
  p.end();
  return QIcon(pm);
}

QIcon metronome(const QColor& color) { return make(IconId::Metronome, color); }
QIcon loop(const QColor& color) { return make(IconId::Loop, color); }
QIcon countIn(const QColor& color) { return make(IconId::CountIn, color); }
QIcon settings(const QColor& color) { return make(IconId::Settings, color); }
QIcon audio(const QColor& color) { return make(IconId::Audio, color); }
QIcon question(const QColor& color) { return make(IconId::Question, color); }

} // namespace misuc::ui::icons