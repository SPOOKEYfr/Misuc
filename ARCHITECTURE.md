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

> Known wrinkle (to be cleaned up if sanitizers flag it): `setMasterGainDb`,
> `setTestTone` and `setMonitorGainDb` also mutate fields directly on the UI
> thread in addition to posting a lock-free `ControlMessage` (`setGroove`
> goes through the queue only). The queue is the intended path.
> `closeAudio()` = backend stop; `stop()` = transport stop.

### project

- `Session.{h,cpp}` — the `.misuc` project foundation as JSON
  (`kSchemaVersion = 2`): schemaVersion, app identity, metadata, transport,
  tempo, and tracks. A track carries id/name/mute/gain and a list of
  `NoteEvent`s (pitch, velocity, start/duration in beats). Version-1 documents
  (no tracks) still load. Owned by the main window; persisted through the File
  menu and edited on the Project page and the Editor page.

## UI (`ui/`)

The UI is a Qt 6 Widgets application (`misuc_app`) with a custom-painted
premium dark theme. Layout comprises a menu bar (File / Edit), a top transport
bar, sidebars, status area, and content pages.

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
- **Audio Settings** page drives the live engine: it lists devices (output and
  input) from `PipeWireBackend::enumerateDevices`, offers sample-rate/buffer
  choices, shows latency, toggles the 440 Hz test tone and the groove beat,
  controls master and monitor gain, and shows live output/input meters with
  clip LEDs.

- `MidiScore.h` — `buildMidiScore(Session, score)`: flattens a session into a
  sorted playback score (mute filtering, per-track gain + velocity baked into
  amplitude) for the UI to publish via `AudioEngine::setScore`.

The UI talks only to `AudioEngine` (and through it the backend interface); it
never touches PipeWire directly.

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
        │  + wall-clock preview notes (audition; independent of transport)
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
