# Misuc Architecture

Misuc is a professional, Linux-first digital audio workstation (DAW). This
document describes the overall architecture and the design decisions that
govern the codebase.

## Design principles

1. **Engine / UI separation is absolute.** The audio engine has no knowledge of
   any UI toolkit. It is a plain C++20 static library (`misuc_engine`) that can
   be driven headlessly and unit-tested.
2. **Real functionality over mockups.** Every phase must build, run, and be
   verified before the next one begins. There are no placeholder screens that
   pretend to work.
3. **Realtime safety.** Code running on the audio (realtime) thread must be
   non-blocking and lock-free. Communication with the audio thread happens
   through bounded lock-free queues and atomic state; the realtime thread never
   allocates, locks a mutex, or blocks.
4. **Linux-first.** The primary audio path is native PipeWire. Other backends
   (e.g. ALSA) exist only for diagnostics.

## High-level layout

```
misuc/
├── engine/                     # misuc_engine static library (UI-independent)
│   ├── include/misuc/engine/
│   │   ├── core/               # ring buffer, control stream, logging, JSON
│   │   ├── dsp/                # oscillator, filters, metering, metronome
│   │   ├── transport/          # musical time / position
│   │   ├── audio/              # backend abstraction + PipeWire implementation
│   │   └── project/            # .misuc project (Session) format
│   └── src/                    # implementation
├── ui/                         # Qt 6 Widgets application shell (misuc_app)
├── tests/                      # misuc_tests executable
├── third-party/                # notes on vendored/external components
└── docs                         # this document, ROADMAP, BUILD
```

## Audio engine (`engine/`)

### core

- `RingBuffer.h` — `SPSCRing<T, Capacity>`, a Vyukov-style bounded lock-free
  single-producer/single-consumer ring buffer (power-of-two capacity). Used for
  the CPU→audio control path. Also `MeterSnapshot` and one-pole `Smoother`.
- `ControlStream.h` — `ControlMessage`, `ControlKind`, `TransportAction`, and
  the `AudioRenderer` interface the audio thread calls into each callback.
- `Logger.h` — realtime-safe logging: the audio thread enqueues into a bounded
  lock-free queue (4096 records) and a separate thread drains and writes them.
- `Json.h` — a small self-contained JSON parser/serializer used by the project
  format (no external dependency).
- `UndoStack.h` — generic snapshot-based undo/redo history for copyable
  document types. Bounded (default 50 snapshots); new edits discard the redo
  tail. Backs the Edit menu's Undo/Redo over the session document.

Everything the audio thread touches is passed **by value or piecewise** through
the lock-free `ControlQueue` (`SPSCRing<ControlMessage, 1024>`). The engine drains
this queue at the top of each audio callback and applies changes.

### dsp

- `Oscillator.h` — test-tone / reference oscillators.
- `Dsp.h` — RBJ biquad low/high/band-pass filters, and `ChannelMeter` (peak,
  RMS with per-channel decay, clipping detection) including
  `processInterleaved`.
- `Metronome.h` — count-in and steady-click metronome; `render(outL, outR,
  frames, stride)`.
- `Groove.h` — deterministic drum + bass groove (kick/snare/hat/bass) quantized
  to a 16th-note grid; `render(outL, outR, frames, stride, startFrame)` where
  `startFrame` is the transport position, so the music locks to the playhead.
- `Midi.h` — polyphonic MIDI playback: a bare score entry (`MidiNote`, beats +
  frequency + baked amplitude) and `MidiPlayer`, which re-synthesizes each
  block as a pure function of absolute sample time. No persistent voice state,
  so blocks stitch seamlessly and stop/rewind/loop restart cleanly with zero
  allocation on the audio thread.
- `Midi.h` (playhead preview) — the player renders two timebases: the **song**
  timebase (score, locked to the transport; only when `includeScore`) and the
  **wall** timebase (a monotonic frame clock) on which short audition notes
  (`triggerPreview`) sound even while the transport is stopped. A fixed 8-slot
  ring of previews absorbs flood triggers by overwriting the oldest.
- `ClipPlayer.h` — audio-clip playback: `ClipBuffer` (raw floats, rate,
  channels) and `ClipPlayer`, a sidechain re-synthesizer that renders clip
  sample positions as a pure function of absolute sample time against a
  real-time-safe snapshot (`ClipSlot` ×4 with rebound pointers), resampling to
  the output rate and gating amplitude by the owning track. Stateless across
  blocks like `MidiPlayer`, so rewind/loop/stop restart cleanly.
- `WavFile.{h,cpp}` — libsndfile-backed WAV I/O: `readAudioFile` (format,
  frame count, sample rate, channels, float samples) and `writeWavSamples`
  (PCM16 or IEEE-float little-endian). Used by record capture and clip
  import.

### transport

- `Transport.h` — play / pause / stop, loop, BPM (clamped [20, 400]) and time
  signature, `barsBeats`, and `formatPosition` producing the 1-based
  `bar.beat.sixteenth.subtick` string.

### audio

- `AudioBackend.h` — device/config/latency/status structs and the abstract
  backend interface. The engine consumes this interface; it never mentions
  PipeWire.
- `PipeWireBackend.{h,cpp}` — the native backend:
  - Node enumeration through the registry (filtering for `Audio/Sink`,
    `Audio/Sink/Virtual`, `Audio/Source`, `Audio/Source/Virtual`, excluding
    monitor ports) for the device picker (both output and input).
  - **Dual-stream** operation from a single `pw_thread_loop`: one playback
    stream and, when `AudioConfig::inputDeviceId` is set, one capture stream.
    Each stream carries its own `StreamSlot` (stream/hook/context/channels) and
    `StreamCtx` (with an `input` flag). Wire format F32 interleaved, flags
    `PW_STREAM_FLAG_AUTOCONNECT | INACTIVE | MAP_BUFFERS | RT_PROCESS`.
  - Xrun and delay tracking via `pw_stream_get_time_n` per stream; input delay
    feeds `LatencyInfo::inputMs` / `roundTripMs`.
  - Planar→interleaved scatter with scratch buffers (`8192 × 8`); capture is
    interleaved into a scratch buffer and handed to the engine's process()
    input path.
  - Teardown is lock-free and async-safe: deactivate under the loop lock,
    `pw_thread_loop_stop()` without the lock (quiesce), then destroy stream /
    hook / context / loop, then free the callback contexts.
- `AudioEngine.{h,cpp}` — implements `AudioRenderer`, owns the streams on the
  backend, drains the lock-free control queue, drives `PlayState`
  (Stopped/Counting/Playing), the count-in metronome, the 440 Hz test tone,
  the transport-quantized groove, master-gain and monitor-gain smoothing
  (one-pole coefficient 0.004), mixes monitored input into the output bus, and
  produces L/R output + input `MeterSnapshot`s.
  - **Per-track mixer stage** (`MixerTrackParam`: gainDb, pan, mute, solo):
    notes and clips enter per-track staging buffers, then each track passes
    through a smoothed gain fader and a constant-power `dsp::panLaw` (centre =
    0.707 per channel) into a common output bus. Separate `Smoother`s are
    created per track on construction (coefficient 0.004, gain reset 1.0,
    panL/panR 0.707106781), so mixer edits land sample-accurately without
    re-scanning notes or re-publishing scores.
  - Any-solo gating: a track is audible iff `!mute && (!anySolo || solo)`.
    Per-track meters measure **post-gain, pre-mute/solo** signal (the real
    audible level) and are polled through non-blocking atomics
    (`trackMeterPeakL/R`, `trackMeterRmsL/R`, `trackMeterClipped`);
    `kMaxTrackCount = 32`. The per-track section of `process()` runs in
    `kMaxBlockFrames = 8192` chunks so huge offline/export blocks never
    allocate on the audio thread. Export (`renderToWav`) flows through the same
    live `process()` path, so the bounce honors the mixer exactly.
  - `publishMixer(...)` / `clearMixer()` carry the mixer params through the
    existing lock-free control queue from the UI thread.

> Known wrinkle (to be cleaned up if sanitizers flag it): `setMasterGainDb`,
> `setTestTone` and `setMonitorGainDb` also mutate fields directly on the UI
> thread in addition to posting a lock-free `ControlMessage` (`setGroove`
> goes through the queue only). The queue is the intended path.
> `closeAudio()` = backend stop; `stop()` = transport stop.

### project

- `Session.{h,cpp}` — the `.misuc` project foundation as JSON
  (`kSchemaVersion = 2`): schemaVersion, app identity, metadata, transport,
  tempo, and tracks. A track carries id/name/mute/gain and a list of
  `NoteEvent`s (pitch, velocity, start/duration in beats). A track may also
  carry an ordered list of `AudioClip`s (name, source file, start/duration in
  beats, sample rate, channels); the field is optional so version-1
  documents (no tracks) and v2 documents without clips still load.
  **Portable asset paths** (Item #5): `Session::relativizeAssets(baseDir)` /
  `resolveAssets(baseDir)` convert every clip path that lives under `baseDir`
  between absolute and project-relative form, so a saved `.misuc` travels
  with its `<folder>/audio/` assets. MainWindow writes a relativized *copy* on
  save (the live session keeps absolute paths for playback) and resolves
  relative references back to absolute paths right after loading. Files
  outside the project folder stay absolute so they remain valid wherever the
  project lands.
- `MidiScore.h` / `ClipScore.h` — `buildMidiScore(Session, score)` flattens a
  session into a sorted playback score; `buildClipScore(Session, clips)` sorts
  active clips by start beat and bounds them against a time horizon. Notes and
  clips carry the id of their source track and a **raw** amplitude only
  (`MidiNote.gain` = velocity/127, `PlaybackClip.gain` = clip gain); track
  gain/pan/mute/solo are applied by the engine's mixer stage, not baked into
  the score. The UI publishes both through `AudioEngine::setScore`/`setClips`;
  the slots partition arrivals into per-track vectors on the publisher thread
  (`setClips` moves the sample storage in and rebases pointers, which is
  identity because a `std::vector` move preserves the allocation address).

## UI (`ui/`)

The UI is a Qt 6 Widgets application (`misuc_app`) with a custom-painted
premium dark theme.

### Modular workspace shell (Phase 13)

`MainWindow` is a `QMainWindow` arranged as a modular DAW shell:

- **Global header** (`HeaderBar`, a non-movable top `QToolBar`): brand cluster
  (logo + MISUC wordmark + live project name) | centred compact `TransportBar`
  | live status cluster (real app CPU % / RAM from `/proc`, sample rate,
  buffer + latency, xruns, transport state, master mini meters).
- **Central workspace**: a `QTabWidget` holding Project / Editor / Audio /
  Appearance; page creation order (Project before Editor) preserves the
  editor track-list as the window's first `QListWidget`.
- **Dockable Mixer**: the mixer lives in a closable/floatable/movable
  `QDockWidget` (bottom by default), shareable with future panels.
- **HintBar** (`HintSystem`): a per-widget `HintManager` event filter +
  registry surfaces name / description / live value / shortcut in the
  `QStatusBar` hint label, refreshed on the 30 Hz UI poll.
- **Workspace menu**: Default / Mixing / Piano Roll / Recording presets plus a
  Mixer toggle; dock layout, active tab, and window size persist via
  `QSettings`. Shortcuts: F7 Piano Roll, F9 Mixer toggle, Space play/stop.
- Transport "click" is a real metronome: `countInToggled` → `engine_.setMetronome`.

Screens refer only to theme tokens; a palette switch re-colors the whole app.

### Main window menus and pages

- **File menu**: New / Open... / Save / Save As... / Quit. The main window owns
  the `Session` document and a `core::UndoStack<Session>`; the Project page and
  the file menu operate on that one shared document. Window title shows
  `name * - Misuc` while there are unsaved changes, with a Save / Discard /
  Cancel guard on New / Open / Quit.
- **Edit menu**: Undo / Redo (`Ctrl+Z` / `Ctrl+Shift+Z`) over snapshots of the
  session.
- **Editor** page: track list (add / remove / mute) plus a custom-painted
  piano-roll editor bound to the selected track of the shared `Session`.
  Ruler, piano-key column, and note grid are separate widgets; edits
  (double-click adds, drag moves, right-click / Del deletes, Ctrl+wheel zooms,
  snap 1/16 toggle) mutate the session directly and emit `edited()` so the
  main window pushes one undo snapshot per gesture.
- **Editor** page (Phase 7): the main window's 30 Hz `poll()` feeds the
  transport playhead (frames → beats via tempo) to the piano roll, which draws
  a green caret on ruler + canvas and auto-follows it while playing. Note
gestures emit `PianoRoll::audition(freq, velocity)` (add / drag / click),
   forwarded to `AudioEngine::previewNote` so notes make sound immediately —
   even while the transport is stopped.
- **Editor** page (Phase 8): scrolling the wheel over an existing note edits
  its velocity (auditioned live at the new gain, committed via `edited()` after
  a 280 ms debounce so one scroll burst = one undo snapshot); each note draws a
  vertical velocity bar at its left edge, and its fill opacity rises with
  velocity. Dragging a note's right-edge grip (6 px, resize cursor on hover)
  resizes its duration snapped to the grid, clamped to 0.1 beats and the
  horizon, committing one undo snapshot per gesture.
- **Editor** page (Phase 8): left-drag on empty canvas rubber-bands a selection
  (dashed marquee, tolerant to drag direction, never auditions or commits);
  dragging any note in a multi-selection moves the whole group by the same
  snapped delta and Del/Backspace deletes it as one undo step.
- **Editor** page (Phase 8): `Ctrl+C` copies the selection, `Ctrl+V` pastes it
  anchored at the visible playhead (or one grid step right of the copy origin),
  and `Ctrl+D` duplicates it; each paste commits one undo snapshot and selects
  the pasted notes. A `Quantize` button (or `Q`) snaps the selection — or all
  notes when none are selected — to the 1/16 grid in one undo step.
- **Transport** (Phase 8): a `LOOP` control (`ControlKind::Loop`,
  `AudioEngine::setLoop`) carries enabled + start/end beats onto the audio
  thread, which maps them to transport frames and drives the existing
  `Transport::advance()` loop wrap. The region is persisted in the Session and
  surfaced on the piano-roll ruler (green shaded region, drag to redraw) and a
  transport-bar `LOOP` toggle.
- **Editor** page (Phase 8): the piano roll adds a 28 px audio-clip lane above
  the note grid. Clips draw as waveform strips (peak-binned per 16th, cached
  by content signature), press-drag un-snaps and moves one across the timeline
  (committed on release as one undo snapshot), and Del/Backspace or
  right-click delete it. Selecting a clip never starts note gestures; all note
  rows are shifted down by the lane and the piano-key ruler grows to match.
  Clip editing on the lane (Phase 8+): a right-edge grip (6 px, `SizeHorCursor`)
  stretches `durationBeats` keeping the left edge and `clipOffsetBeats` fixed,
  a left-edge grip trims-in keeping the right edge fixed while pushing
  `clipOffsetBeats` deeper into the source, Ctrl+D duplicates the selected clip
  one snap step to the right (fresh id, inheriting offset/gain/edges), and the
  wheel over a clip nudges clip `gainDb` by ±1 dB. Each gesture commits as a
  single undo snapshot (gain uses the same debounce timer as note velocity).
- **Import / record** (Phase 8): an "Import Audio…" button reads
  WAV/FLAC/OGG/AIFF through `WavFile::readAudioFile`, places the clip at the
  current playhead of the active track (staging the file into the project's
  `audio/` folder when a project is open), and emits `edited()` once. A record
  button on the transport bar arms capture from the playhead and, on stop,
  writes the take as a mono WAV under `<project>/audio/clip_NNN_<epoch>.wav`
  (temp dir when no project) and lands an `AudioClip` on the session. Every
  path into the session (edit, undo/redo, new/open) calls
  `rebuildClipScore()` → `buildClipScore` + `AudioEngine::setClips`, so clips
  follow the transport live like notes.
- **Audio Settings** page drives the live engine: it lists devices (output and
  input) from `PipeWireBackend::enumerateDevices`, offers sample-rate/buffer
  choices, shows latency, toggles the 440 Hz test tone and the groove beat,
  controls master and monitor gain, and shows live output/input meters with
  clip LEDs.
- **Mixer** page (`ui/src/mixer/`) — the real mixer panel: one `ChannelStrip`
  per track (color dot, pan `Knob` −100…+100, M/S buttons, vertical stereo
  `VerticalMeter` with RMS fill / peak-hold cap / clip LED, dB `Fader` −60…+12
  with wheel fine-tune and double-click-to-0, plus a gain readout) and a Master
  strip (fader + stereo meter) at the right. Strips are plain views over the
  shared Session: user edits become session mutations via `edited()` and land on
  the same undo stack as every other edit. `syncToEngine()` re-publishes the
  track mixer params (`publishMixer`) from every session change, and
  `pollMeters()` (on the main window's 30 Hz tick) pulls the per-track physics
  from the engine atomics. Track selection is mirrored both ways between the
  editor's track list and the strips. The `MixerPage` rebuilds its rack when the
  track set changes and otherwise only mirrors values, so a fader drag is never
  interrupted.
- **Appearance** page (`ui/src/theme/ThemePage.{h,cpp}`) — presets, custom
  accent, glass toggle, Save/Load `.misuctheme`, all applied live through the
  theme engine.
- **Theme engine** (`ui/src/theme/ThemeEngine.{h,cpp}`) — owns the `theme::k*`
  token set (mutable inline `QColor` globals, so API call sites are unchanged),
  provides eight factory presets, recomputes derived accent tokens on a custom
  accent, builds the app palette + stylesheet, persists the choice via
  `QSettings`, and round-trips `.misuctheme` files. `apply(QApplication&)`
  emits `themeChanged()` so live widgets (like the nav-logo) restyle.
- **Branding** (`ui/src/branding/Logo.{h,cpp}`) — shared vector logo (a gold
  MIDI note whose stem blooms into a flame) painted in `QPainter`; the nav-rail
  mark and window icon draw from the active theme accent at runtime, while the
  on-disk icon set is regenerated from the same code by `tools/rendericons.cpp`
  into hicolor PNGs (16–256 px) plus a bundled `:/icons/misuc-256.png`. A
  `.desktop` entry and `cmake install` rules stage the app, entry, and icon
  theme into the standard `share/` locations.

The UI talks only to `AudioEngine` (and through it the backend interface); it
never touches PipeWire directly.

### Modular workspaces (Phase 14)

The Phase-13 shell grew from three fixed pages into a managed set of
workspaces, all owned by one object:

- **`ui/src/workspace/WorkspaceManager.{h,cpp}`** — owns the central
  `QTabWidget` (Playlist, Piano Roll, Project, Audio, Appearance) and every
  `QDockWidget` (Browser left, Mixer bottom, Inspector + Channel Rack right)
  with `addCentral` / `addDock`, `widget()` / `docks()`, `isVisible()`,
  `showWorkspace()` / `toggleWorkspace()`, six presets (Default / Playlist /
  Piano Roll / Channel Rack / Mixing / Recording) that hide-all-then-show the
  relevant docks and switch the central tab, and QSettings persistence of the
  dock state (through `QMainWindow::saveState`), active central tab, and
  preset. Signals: `workspaceToggled(Id, bool)` and `centralChanged(Id)`.
- **`ui/src/playlist/Playlist.{h,cpp}`** — arrangement page. `PlaylistGrid`
  paints the time ruler, per-track lanes (mute/solo dots in the label column),
  audio clips with real decoded waveform peaks (`<sndfile.h>` windowed 256×256
  sampling, cached in a static QHash), and MIDI regions derived straight from
  each track's `NoteEvent`s. Selection, drag-move/resize (beat-snapped),
  split-at-playhead, duplicate, delete, rename, context menu, Browser
  drop-import, Import Audio, and Select / Slice / Snap toolbar tools. It keeps
  the engine transport position and renders the shared violet playhead. Exposes
  `SelectionSummary` for the Inspector and mirrors selection to the Editor and
  Mixer.
- **`ui/src/channel/ChannelRack.{h,cpp}`** — step sequencer as QTableView +
  QStandardItemModel with a custom delegate (gated cells accent-filled, mute /
  solo, volume bar + %, pan). Persisted channel snapshots go to QSettings
  (`ui/channelRack`, base64 step masks); not part of the undo/session model.
- **`ui/src/browser/Browser.{h,cpp}`** — left dock. Category tree (All
  Samples, Current Project, Instruments, Audio Effects, MIDI, Plugins),
  recursive real-file scan (capped), search filter, on-demand decode preview
  via `misuc::audio::readAudioFile`, and two ways to insert a sample into the
  Playlist (drag-and-drop and Insert). Plugin category entries are
  informational text rows only — no fake DSP.
- **`ui/src/inspector/Inspector.{h,cpp}`** — right dock. Read-only context
  panel (workspace intro / track / audio clip / MIDI region / note) driven by
  `Playlist::selectionSummary()`; owners mutate their own data, so the
  Inspector is a faithful mirror by design.
- **Commands** (`ui/src/commands/MisucCommandRegistry`) and **tooltips**
  (`ui/src/tooltip/MisucTooltip`) stay the single source of truth for menus,
  header buttons, and hint text; `buildCommands()` in MainWindow registers
  workspace, transport, metronome, snap, and song/pattern commands.
- Session mutation flows through `Session::tracks()` (mutable reference); the
  engine API is untouched. Clip mute is a `gainDb=-96` convention, and dropped
  Browser samples reference their original absolute paths (asset staging is
  still used for editor imports).

## Data flow (audio thread)

```
PipeWire process callback(s)
        │
        ▼
AudioEngine::process (AudioRenderer; input + output)
        │  drain ControlQueue (lock-free, from CPU thread)
        │  count-in metronome / test tone / groove (transport-quantized)
        │  MIDI score player (stateless re-synthesis from a real-time-safe
        │  triple-buffer snapshot published by the UI on every edit)
        │  + ClipPlayer sidechain (audio-clip slots from AudioEngine::setClips)
        │  + wall-clock preview notes (audition; independent of transport)
        │  per-track mixer stage: smoothed gain → constant-power pan →
        │  mute/solo (anySolo) gating → common bus; track meters (post-gain,
        │  pre-gate) via atomics
        │  monitored input → monitor gain smoother → output bus
        │  master-gain smoothing
        │  gather output + input MeterSnapshot
        ▼
PipeWireBackend scatter → interleaved F32 buffer → pw_stream(s)
```

## Build & test

See BUILD.md. Core workflow:

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```
