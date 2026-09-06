#pragma once

#include <QColor>
#include <QFont>

namespace misuc::ui {

// Design system for the Misuc interface. A warm, deep studio palette with a
// single actionable accent. All colors are frozen here so screens stay
// consistent.

namespace theme {

inline const QColor kBackground("#0d121b");
inline const QColor kPanel("#151c27");
inline const QColor kPanelRaised("#1c2531");
inline const QColor kPanelSunken("#0a0e15");
inline const QColor kBorder("#28303e");
inline const QColor kBorderBright("#354054");

inline const QColor kText("#e8edf4");
inline const QColor kTextDim("#8a94a6");
inline const QColor kTextFaint("#5b6472");

inline const QColor kAccent("#ff6b4a");       // warm coral
inline const QColor kAccentPressed("#e85533");
inline const QColor kAccentSoft("#3a2420");
inline const QColor kOk("#4ade80");
inline const QColor kDanger("#ff5c5c");
inline const QColor kMeterDim("#1f2836");
inline const QColor kMeterTrack("#263041");

QFont uiFont(int pointSize = 10, QFont::Weight weight = QFont::Normal);
QFont monoFont(int pointSize = 10);

} // namespace theme

} // namespace misuc::ui