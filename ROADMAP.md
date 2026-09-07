# Misuc Roadmap

Every phase must result in something that **builds, runs, and is verified**
before the next phase starts. Milestones below are the current plan; they will
be refined as development proceeds.

## Phase 0 — Foundation (done)

- Environment: Linux Mint 22.3, g++ 13.3 (C++20), CMake 3.28, Ninja.
- Dependencies: `qt6-base-dev`, `libpipewire-0.3-dev`, `libasound2-dev`,
  `libsndfile1-dev`.
- Repo scaffolding: root CMake, `.gitignore`, `third-party/README.md`.
- Design principles written down (see ARCHITECTURE.md).

## Phase 1 — Engine core (done)

- Lock-free realtime infrastructure: `SPSCRing`, `ControlStream`,
  realtime-safe `Logger`.
- DSP primitives: oscillator, biquad filters, metering, metronome.
- Musical `Transport` (transport, tempo, loop, format position).
- Own JSON + `.misuc` `Session` foundation.
- Unit-test suite (`misuc_tests`): 1863 checks, all passing.
- Live PipeWire device enumeration verified on this machine (Built-in Audio
  Analog Stereo sink/source).

## Phase 2 — App shell (done)

- Qt 6 Widgets app `misuc_app` with a custom-painted premium dark theme.
- Main window: transport bar, work area, sidebars, status area.
- Transport bar: play / stop with count-in toggle, tempo + time-signature
  readout, and the formatted bar.beat.sixteenth.subtick playhead.
- **Audio Settings** page: real device list (from the running engine's
  backend), sample-rate and buffer-size choices, latency display, 440 Hz test
  tone start/stop, master gain, live stereo meters and clip LED.
- **Project** page: edit name/tempo and save/open `.misuc` (real JSON
  sessions).
- Status strip with engine state, negotiated audio, xruns and wall clock.
- PipeWire audio lifecycle hardened: enumeration and stream teardown no longer
  deadlock or corrupt the heap (regression test `stream_lifecycle`); verified
  with repeated open/start/stop cycles at 48 kHz.
- Full suite builds green: `misuc_tests` 1869 checks, 0 failures; app runs
  cleanly on this machine.

## Phase 3 — Real audio output & input (done)

- PipeWire dual-stream backend: output + optional capture, run from the same
  `pw_thread_loop`. Stream config respects `inputChannels` / `inputDeviceId`.
- Input monitoring: audio routed to output meters while transport runs, with a
  separate monitor-gain control (range −60 to +6 dB).
- Input metering (peak / RMS / clip) exposed through `AudioEngine` and shown in
  the Audio Settings page alongside the output meter.
- `Groove` synthesizer: deterministic kick/snare/hat/bass drum+synth pattern
  locked to the transport position; toggled with a checkbox and audible through
  the master output.
- `AudioRenderer::process` extended with `(input, inputChannels, …)`.
- `MeterSnapshot` extended with `inPeak / inRms / inClipped` fields.
- `LatencyInfo` extended with `inputMs` and `roundTripMs`.
- UI: input device picker (includes "None" option), monitor-gain slider,
  monitor toggle, groove checkbox, input meter, latency display now shows
  input delay.
- Headless verification: open with `inputDeviceId=55`, input meter reads real
  mic level (~0.2 peak), output peak from groove ~2.2, latency 385 ms
  (large quantum). Teardown clean.
- New groove unit tests: disabled = silent, hits carry energy, stride-1 vs
  stride-2 render identical; live groove-and-capture lifecycle regression
  added. Full suite stays green: 49874 checks, 0 failures.

## Phase 4 — Project workflow

- File menu: New / Open / Save / Save As for `.misuc`.
- Session persistence via JSON; undo stack foundation.

## Phase 4 — Project workflow (done)

- File menu: New / Open... / Save / Save As... / Quit (with `Ctrl+N/O/S/⇧S/Q`).
- Unsaved-changes guard on New / Open / Quit (Save / Discard / Cancel).
- The main window owns the session document; the Project page edits it; the
  window title shows `name * - Misuc` while dirty.
- Undo / Redo (Edit menu, `Ctrl+Z` / `Ctrl+⇧Z`) on a snapshot-based core
  `UndoStack<T>`; bounded history, redo tail dropped on new edits. Unit-tested
  (round-trip, branch truncation, capacity, clear).
- Session tempo changes are pushed to the running engine from any entry point
  (page edit, undo/redo, open, new).

## Phase 5 — Core UI/peripherals

- Arrangement/mixer themes, piano roll rendering, timeline, MIDI note entry.
- Undo/redo for editing, snap settings, zoom.

## Phase 5 — Core UI/peripherals (done)

- Session model extended to v2: `NoteEvent` (channel, pitch, velocity,
  start/duration in beats) and `Track` (id, name, muted, gainDb, notes), both
  serialized to JSON with v1 backward compatibility.
- Editor page: track list with Add/Remove + per-track mute, zoom slider
  (10–160 px/beat), snap 1/16 toggle, and a piano-roll editor bound to the
  selected track of the shared Session.
- Piano roll: ruler + piano-key column + scrollable note grid (61 pitches,
  C1..C6, 64-beat horizon); double-click empty adds a snapped note, drag moves
  it (time + pitch), right-click/Del deletes, Ctrl+wheel zooms.
- Every edit (`edited()`) lands on the shared undo stack — the same
  snapshot-based UndoStack from Phase 4 — so all piano-roll gestures and track
  changes are undoable from the Edit menu.
- Verified headless with synthetic events: 22 behavioral assertions covering
  add/drag/delete/snap/zoom/track-lifecycle/mute all pass; full suite green at
  49944 checks, 0 failures.

## Phase 6 — MIDI playback

- Notes drawn in the piano roll make sound while the transport runs.

## Phase 6 — MIDI playback (done)

- New `dsp::MidiPlayer`: polyphonic subtractive MIDI voices re-synthesised as
  a pure function of absolute sample time — no persistent voice state, so
  blocks stitch seamlessly and stop/rewind/loop restart cleanly with zero
  allocation on the audio thread.
- Real-time-safe score handoff: the UI flattens the Session
  (`buildMidiScore`: mute filtering, per-track gain + velocity baked into
  amplitude) and publishes a snapshot through `AudioEngine::setScore` into a
  triple buffer; the audio thread holds one slot per block, the publisher
  round-robins three.
- Score republished on every edit, undo/redo, open, and new — live edits
  follow the transport instantly (mute, gain, moving notes).
- Unit tests cover pitch→freq, attack/release envelope, seamless block
  boundaries (bit-identical), deterministic rewinds, polyphonic summation,
  tempo scaling, and score mapping; a live daemon test confirms MIDI-alone
  playback is audible. Suite: 53989 checks, 0 failures.

## Phase 7+ — Beyond

- VST3/LV2 plugin hosting, buses and routing, recording, rendering, GPU
  acceleration for the UI when it pays off.

## Status conventions

- `done` — builds, runs, and tests pass.
- `current` — being implemented in this session.
- Anything "planned" stays a bullet above; nothing is marked done until it is
  actually verified.