#pragma once

#include <QColor>
#include <QIcon>

namespace misuc::ui::icons {

// Themeable vector glyphs for the Misuc interface. All glyphs are drawn as
// QPainter paths so they re-render automatically when the theme changes, and
// never ship as bitmaps. Keep legacy free functions for stable branding calls;
// prefer `make(IconId, ...)` for registry/menu/toolbar icons.

enum class IconId {
  None,
  App,
  Playlist,      // arrangement workspace
  PianoRoll,     // MIDI editor workspace
  ChannelRack,   // step-sequencer workspace
  Mixer,         // mixer workspace
  Browser,       // file/sample browser workspace
  Inspector,     // context properties panel
  Plugins,       // plugin manager
  Play,
  Stop,
  Record,
  Metronome,
  CountIn,
  Loop,
  Snap,
  SongMode,
  PatternMode,
  Select,
  Draw,
  Paint,
  Erase,
  Slice,
  ZoomIn,
  Mute,
  Solo,
  Arm,
  VisibilityOn,
  VisibilityOff,
  Options,
  Instruments,
  Audio,
  Midi,
  Samples,
  Effects,
  Patterns,
  Automation,
  AllItems,
  CurrentProject,
  Favorites,
  Recent,
  Search,
  Add,
  Remove,
  Duplicate,
  Rename,
  Folder,
  ChevronRight,
  ChevronDown,
  NewDoc,
  OpenDoc,
  SaveDoc,
  ExportDoc,
  Undo,
  Redo,
  Close,
  Pin,
  Float,
  Preview,
  Question,
  Settings,
  Waveform,
  Refresh,
};

QIcon make(IconId id, const QColor& color, int size = 18);

// Legacy branding glyphs (kept for API stability).
QIcon metronome(const QColor& color);
QIcon loop(const QColor& color);
QIcon countIn(const QColor& color);
QIcon settings(const QColor& color);
QIcon audio(const QColor& color);
QIcon question(const QColor& color);

} // namespace misuc::ui::icons