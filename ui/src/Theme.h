#pragma once

#include <QColor>
#include <QFont>

namespace misuc::ui {

// Design system for the Misuc interface. The token values below are the
// "Misuc Graphite" default palette; the theme engine swaps them whenever a
// preset or a custom accent is applied, and every widget that paints reads
// them at paint time so the whole app re-colors on `ThemeEngine::apply`.
// Screens must never hard-code colors — always reference these tokens.

namespace theme {

inline QColor kBackground("#0d121b");
inline QColor kPanel("#151c27");
inline QColor kPanelRaised("#1c2531");
inline QColor kPanelSunken("#0a0e15");
inline QColor kBorder("#28303e");
inline QColor kBorderBright("#354054");

inline QColor kText("#e8edf4");
inline QColor kTextDim("#8a94a6");
inline QColor kTextFaint("#5b6472");

inline QColor kAccent("#ff6b4a");       // warm coral
inline QColor kAccentPressed("#e85533");
inline QColor kAccentSoft("#3a2420");
inline QColor kOk("#4ade80");
inline QColor kDanger("#ff5c5c");
inline QColor kMeterDim("#1f2836");
inline QColor kMeterTrack("#263041");

// Glass material: a translucent raised surface with a bright top edge, used by
// the mixer panels. Falls back to opaque kPanelRaised when glass is disabled
// (software rendering / weak GPU) so the UI stays fast and legible.
inline QColor kGlass(0x22, 0x2e, 0x40, 200);
inline QColor kGlassBorder(0x88, 0x9a, 0xb5, 70);
inline QColor kGlassHighlight(0xff, 0xff, 0xff, 26);

QFont uiFont(int pointSize = 10, QFont::Weight weight = QFont::Normal);
QFont monoFont(int pointSize = 10);

} // namespace theme

} // namespace misuc::ui