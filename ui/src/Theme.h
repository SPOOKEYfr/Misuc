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

inline QColor kAccent("#8f7bfa");       // restrained violet
inline QColor kAccentPressed("#6f5cd6");
inline QColor kAccentSoft("#2a2350");
inline QColor kOk("#4ade80");
inline QColor kDanger("#ff5c5c");
inline QColor kMeterDim("#1f2836");
inline QColor kMeterTrack("#263041");

// Glass material: a translucent raised surface with a bright top edge, used by
// the mixer panels. Falls back to opaque kPanelRaised when glass is disabled
// (software rendering / weak GPU) so the UI stays fast and legible.
inline QColor kGlass(0x22, 0x2e, 0x40, 186);
inline QColor kGlassBorder(0x88, 0x9a, 0xb5, 78);
inline QColor kGlassHighlight(0xff, 0xff, 0xff, 26);

// --------------------------------------------------------------------------
// Phase 1 semantic tokens (charcoal-black glass design system). Every widget
// must reference these; literals are not allowed in screens. The values below
// are the "Misuc Graphite" stock palette; ThemeEngine overwrites the
// theme-driven subset on every apply so a preset/accent switch re-colors the
// whole app. The editor-neutral tokens (playhead, loop, clip, grid, meters)
// keep fixed stock values so compositions stay readable in any theme.
// --------------------------------------------------------------------------

// Surfaces: layered charcoal workspace with elevated/glass panels on top.
inline QColor kBackgroundSecondary("#0e131c");
inline QColor kSurface("#151c27");           // == kPanel
inline QColor kSurfaceElevated("#1c2531");   // == kPanelRaised
inline QColor kSurfaceGlassStrong(0x2a, 0x34, 0x46, 235);
inline QColor kBorderSubtle(0x20, 0x29, 0x3a, 150);

// Text hierarchy.
inline QColor kTextSecondary("#a9b4c5");
inline QColor kTextMuted("#5b6472");         // == kTextFaint
inline QColor kTextDisabled("#3d4450");

// Interactive accents (violet design language, §37).
inline QColor kAccentHover("#aa96ff");
inline QColor kAccentActive("#bdabff");
inline QColor kAccentFaint(0x8f, 0x7b, 0xfa, 38);
inline QColor kSelection("#2a2350");          // == kAccentSoft
inline QColor kMatteSurface("#111722");       // matte charcoal for racks/playlist

// Chrome surfaces (header / dock titles / transport / hint bar).
inline QColor kPanelHeader("#161c27");
inline QColor kToolbar("#0c1017");
inline QColor kTransport("#0d121b");
inline QColor kHintBar("#0d121b");
inline QColor kTooltipBg("#1f2430");
inline QColor kButton("#1d2531");
inline QColor kButtonHover("#283245");
inline QColor kButtonPressed("#141a24");
inline QColor kBorderStrong("#3a4660");

// Matte-glass chrome layer (spec §36–§38, §46). The app is drawn over a
// near-black charcoal base; every chrome surface (header, dock chrome,
// toolbars, hint bar, tab/menu chrome) is a translucent matte charcoal that
// composites over it, giving the DAW its smoked-glass depth without needing a
// compositor. Values here are the Graphite stock; ThemeEngine derives them
// from the active palette on every apply so glass follows the theme.
inline QColor kBackdrop("#0a0913");        // charcoal-black base, violet undertone
inline QColor kChrome(0x0d, 0x10, 0x19, 226);    // matte glass surface
inline QColor kChromeRaised(0x15, 0x1a, 0x27, 234); // raised glass (chrome front)
inline QColor kChromeBorder(0x66, 0x74, 0x99, 92); // faint violet-blue seams
inline QColor kChromeGlowTop(0xff, 0xff, 0xff, 20); // top-edge specular

// Editor + meters (fixed stock values so pixel tests and readability hold).
inline QColor kGridRow("#232c3b");
inline QColor kGridStrong("#3a4660");
inline QColor kPlayhead(0xb3, 0x9b, 0xff, 255);   // thin luminous violet caret
inline QColor kLoopRegion(0x8f, 0x7b, 0xfa, 90);  // translucent violet loop
inline QColor kClipBody("#1d3347");
inline QColor kClipMidi("#223059");
inline QColor kClipPattern("#2a2350");
inline QColor kClipAutomation("#1d3d34");
inline QColor kClipWave("#6fb5de");
inline QColor kMarquee("#9ca3af");
inline QColor kMeterNormal("#4ade80");
inline QColor kMeterHot("#ffb454");
inline QColor kMeterClip("#ff5c5c");

QFont uiFont(int pointSize = 10, QFont::Weight weight = QFont::Normal);
QFont monoFont(int pointSize = 10);

// Returns a copy of `color` with the requested alpha (Qt 6.4 has no
// QColor::withAlpha yet).
inline QColor withAlpha(QColor color, int alpha) {
  color.setAlpha(alpha);
  return color;
}

} // namespace theme

} // namespace misuc::ui