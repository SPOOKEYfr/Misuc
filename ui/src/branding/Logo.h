#pragma once

#include <QColor>
#include <QPixmap>

class QPainter;
class QRectF;

namespace misuc::ui {

// Shared vector logo: a MIDI note whose stem blooms into a flame, drawn onto a
// rounded "glass" tile. The core logo path is normalized to a 1x1 box.
// `drawLogo` paints the flame + note alone (transparent); `logoPixmap` wraps it
// in the branded rounded tile used for the window icon and the nav mark.
// The flame adapts to the active theme accent; the default brand gradient
// (coral -> amber) is used for the desktop icon set on disk.

void drawLogo(QPainter& p, const QRectF& box, const QColor& accent,
              const QColor& accentSoft, const QColor& flameCore);

QPixmap logoPixmap(int size, const QColor& accent, const QColor& accentSoft);

// Default brand colors for the on-disk icon set.
QColor logoAccent();
QColor logoAccentSoft();
QColor logoCore();

} // namespace misuc::ui