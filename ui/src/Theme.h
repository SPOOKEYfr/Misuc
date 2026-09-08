#pragma once

#include <QColor>
#include <QFont>

namespace misuc::ui {

// Design system for the Misuc interface. "Misuc Black" is the signature
// stock palette: near-black foundation, layered matte charcoals, violet
// reserved for accents/states. The theme engine swaps values on preset/accent
// apply, and every widget that paints reads them at paint time so the whole
// app re-colors on `ThemeEngine::apply`. Screens must never hard-code
// colors — always reference these tokens.

namespace theme {

inline QColor kBackground("#0a0a0c");
inline QColor kPanel("#101013");
inline QColor kPanelRaised("#17171b");
inline QColor kPanelSunken("#050506");
inline QColor kBorder("#232328");
inline QColor kBorderBright("#2c2c33");

inline QColor kText("#ececef");
inline QColor kTextDim("#8b8b94");
inline QColor kTextFaint("#56565e");

inline QColor kAccent("#8f7bfa");       // restrained violet
inline QColor kAccentPressed("#6f5cd6");
inline QColor kAccentSoft("#2a2350");
inline QColor kOk("#4ade80");
inline QColor kDanger("#ff5c5c");
inline QColor kDangerSoft(0x3a, 0x14, 0x17); // muted wine behind a combo or mute toggle
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

// Surfaces: layered charcoal-black workspace with elevated/glass panels on top.
inline QColor kBackgroundSecondary("#0d0d10");
inline QColor kSurface("#101013");           // == kPanel
inline QColor kSurfaceElevated("#17171b");   // == kPanelRaised
inline QColor kSurfaceGlassStrong(0x1c, 0x1c, 0x21, 235);
inline QColor kBorderSubtle(0x23, 0x23, 0x28, 150);

// Text hierarchy.
inline QColor kTextSecondary("#a9a9b1");
inline QColor kTextMuted("#56565e");         // == kTextFaint
inline QColor kTextDisabled("#3a3a40");

// Interactive accents (violet design language, §37).
inline QColor kAccentHover("#aa96ff");
inline QColor kAccentActive("#bdabff");
inline QColor kAccentFaint(0x8f, 0x7b, 0xfa, 38);
inline QColor kSelection("#241f3f");          // deep violet-charcoal selection
inline QColor kMatteSurface("#0f0f12");       // matte black for racks/playlist

// Chrome surfaces (header / dock titles / transport / hint bar).
inline QColor kPanelHeader("#121215");
inline QColor kToolbar("#0a0a0c");
inline QColor kTransport("#0a0a0c");
inline QColor kHintBar("#0a0a0c");
inline QColor kTooltipBg("#1b1b20");
inline QColor kButton("#17171b");
inline QColor kButtonHover("#232329");
inline QColor kButtonPressed("#0d0d10");
inline QColor kBorderStrong("#2c2c33");

// Matte-glass chrome layer (spec §36–§38, §46). The app is drawn over a
// true near-black charcoal base; every chrome surface (header, dock chrome,
// toolbars, hint bar, tab/menu chrome) is a translucent matte charcoal that
// composites over it, giving the DAW its smoked-glass depth without needing a
// compositor. Values here are the Black stock; ThemeEngine derives them from
// the active palette on every apply so glass follows the theme.
inline QColor kBackdrop("#08080a");        // charcoal-black base
inline QColor kChrome(0x10, 0x10, 0x13, 226);    // matte glass surface
inline QColor kChromeRaised(0x17, 0x17, 0x1b, 234); // raised glass (chrome front)
inline QColor kChromeBorder(0x44, 0x44, 0x4c, 92); // faint neutral seams
inline QColor kChromeGlowTop(0xff, 0xff, 0xff, 20); // top-edge specular

// Editor + meters (fixed stock values so pixel tests and readability hold).
inline QColor kGridRow("#222226");
inline QColor kGridStrong("#2c2c33");
inline QColor kPlayhead(0xb3, 0x9b, 0xff, 255);   // thin luminous violet caret
inline QColor kLoopRegion(0x8f, 0x7b, 0xfa, 90);  // translucent violet loop
inline QColor kClipBody("#181c22");
inline QColor kClipMidi("#1d2030");
inline QColor kClipPattern("#221e3a");
inline QColor kClipAutomation("#16231f");
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