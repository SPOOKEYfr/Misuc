#pragma once

#include <QColor>
#include <QIcon>

namespace misuc::ui::icons {

// Themeable vector glyphs (QPainter paths) for chrome controls. Callers pass the
// current theme color so icons re-render automatically on theme changes; render
// at 22x22 logical pixels into a translucent-safe pixmap suitable for QIcon.
QIcon metronome(const QColor& color);
QIcon loop(const QColor& color);
QIcon countIn(const QColor& color);
QIcon settings(const QColor& color);
QIcon audio(const QColor& color);
QIcon question(const QColor& color);

} // namespace misuc::ui::icons