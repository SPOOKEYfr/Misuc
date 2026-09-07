#include "Logo.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace misuc::ui {

QColor logoAccent() { return QColor(0xff, 0x6b, 0x4a); }
QColor logoAccentSoft() { return QColor(0xff, 0xb4, 0x54); }
QColor logoCore() { return QColor(0xff, 0xe0, 0x8a); }

// Note gold, warm enough to read as music yet bright next to the flame.
namespace {
const QColor kNoteGold(0xf6, 0xc6, 0x5a);
const QColor kNoteRim(0xff, 0x9f, 0x40);
const QColor kFlameTop(0xff, 0xe0, 0x8a);
}

void drawLogo(QPainter& p, const QRectF& box, const QColor& accent,
              const QColor& accentSoft, const QColor& flameCore) {
  p.save();
  p.translate(box.topLeft());
  p.scale(box.width(), box.height());
  p.setRenderHint(QPainter::Antialiasing);

  // ---- Flame (bleeds above the note, anchored on the stem tip). -----------
  QLinearGradient flameGrad(0, 0.06, 0, 0.36);
  flameGrad.setColorAt(0.0, flameCore);
  flameGrad.setColorAt(0.55, accentSoft);
  flameGrad.setColorAt(1.0, accent);

  QPainterPath flame;
  flame.moveTo(0.455, 0.03);
  flame.cubicTo(0.62, 0.20, 0.60, 0.345, 0.455, 0.315);
  flame.cubicTo(0.485, 0.37, 0.425, 0.375, 0.315, 0.285);
  flame.cubicTo(0.285, 0.225, 0.37, 0.13, 0.455, 0.03);
  flame.closeSubpath();
  p.fillPath(flame, flameGrad);

  // Outer glow.
  QRadialGradient glow(QPointF(0.455, 0.24), 0.28);
  glow.setColorAt(0.0, QColor(accent.red(), accent.green(), accent.blue(), 150));
  glow.setColorAt(1.0, QColor(accent.red(), accent.green(), accent.blue(), 0));
  p.fillRect(QRectF(0.12, 0.0, 0.7, 0.42), glow);

  // Inner core flame.
  QPainterPath core;
  core.moveTo(0.455, 0.12);
  core.cubicTo(0.505, 0.205, 0.495, 0.275, 0.455, 0.265);
  core.cubicTo(0.425, 0.285, 0.405, 0.225, 0.455, 0.12);
  core.closeSubpath();
  p.fillPath(core, flameCore);

  // ---- Note stem ----------------------------------------------------------
  p.setRenderHint(QPainter::Antialiasing, true);
  p.fillRect(QRectF(0.395, 0.30, 0.085, 0.40), kNoteGold);

  // Beam / flag curving down to the right from the stem top.
  QPainterPath flag;
  flag.moveTo(0.42, 0.315);
  flag.cubicTo(0.70, 0.35, 0.68, 0.58, 0.545, 0.56);
  p.setPen(QPen(kNoteGold, 0.055, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.drawPath(flag);

  // ---- Note head (tilted ellipse) ------------------------------------------
  p.save();
  p.translate(0.30, 0.79);
  p.rotate(-28.0);
  const QRectF head(-0.155, -0.115, 0.31, 0.23);
  p.setPen(QPen(kNoteRim, 0.022));
  p.setBrush(kNoteGold);
  p.drawEllipse(head);
  p.restore();

  p.restore();
}

QPixmap logoPixmap(int size, const QColor& accent, const QColor& accentSoft) {
  QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::transparent);

  QPainter p(&img);
  p.setRenderHint(QPainter::Antialiasing);
  const float r = size / 8.f;

  // Rounded "glass" tile.
  const QRectF tile(2.f, 2.f, size - 4.f, size - 4.f);
  p.setPen(QPen(QColor(255, 255, 255, 36), 2.f));
  QLinearGradient bg(0, 0, 0, size);
  bg.setColorAt(0.0, QColor(0x2a, 0x2a, 0x33));
  bg.setColorAt(1.0, QColor(0x19, 0x19, 0x1e));
  p.setBrush(bg);
  p.drawRoundedRect(tile, r, r);

  // Logo at ~78% of the tile.
  const QRectF box = tile.adjusted(size * 0.12f, size * 0.10f, -size * 0.12f,
                                   -size * 0.10f);
  drawLogo(p, box, accent, accentSoft, logoCore());

  // Accent rim on the tile.
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(), 120), 2.f));
  p.drawRoundedRect(tile, r, r);

  p.end();
  return QPixmap::fromImage(img);
}

} // namespace misuc::ui