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

## Phase 7 — Playhead caret & note audition (done)

- Transport playhead in the piano roll: the 30 Hz UI tick converts the engine's
  playhead frames to beats and drives a green caret that scrolls the timeline
  (auto-follow) while playing; drawn on both ruler and note canvas.
- Note audition: `AudioEngine::previewNote(freq, velocity)` feeds a new
  `ControlKind::PreviewNote` control into the real-time queue; the MIDI player
  renders it on a **wall-clock** timebase (a frame counter that advances in
  every process block), so preview notes sound even while the transport is
  stopped, and coexist/track with the score.
- The piano roll emits `audition()` as notes are added, dragged, or clicked;
  the main window forwards the pitch to the engine.
- Fixes surfaced by this work: the player's incremental scan broke block
  rendering (silent notes) — added a shared note-synthesis helper and frame-
  based envelopes; previews now render even with no score slot live.
- New unit tests: preview on the wall clock with no score, decay after
  duration, frequency matches, score+preview phase-add, preview ring
  overwriting; live stopped-preview audible test. Full suite remains green:
  54003 checks, 0 failures. Offscreen UI harness verifies the caret render and
  audition signals (37 checks).

## Phase 8 — Note velocity editing (done)

- Each note in the piano roll draws a small velocity bar at its left edge whose
  height is proportional to the note's velocity; the note's fill opacity also
  tracks it.
- Scrolling the wheel over an existing note adjusts its velocity (one notch ≈
  5), auditioning the note at the new velocity in real time; the change is
  committed to the undo stack once the scroll gesture settles (280 ms debounce)
  so a single burst produces exactly one snapshot.
- Velocity is bounded to 1..127 and already flows into playback gain and
  audition gain. No engine or session changes were needed.
- UI harness (57 checks): wheel-down floors velocity at 1, wheel-up ceilings at
  127, audition reports the updated velocity, a debounced `edited()` fires, and
  the velocity bar grows measurably in the rendered pixels (27 bar pixels at
  max vs 0 at min).
- The harness no longer links Qt6Test: that library crashes this Qt 6.4.2 stack
  at `QApplication` construction (a Qt bug), so event synthesis and signal
  spies are hand-rolled and the test pins `-fPIC` to match the UI objects.
  Full engine suite unchanged: 54003 checks, 0 failures.

## Phase 8 — Note duration resizing (done)

- Dragging a note's right edge resizes its duration (snapped to 1/16 while snap
  is on, clamped to a 0.1-beat minimum and to the horizon). A 6 px grip band at
  the right edge of the note (with a resize cursor on hover) decides between
  resize and move; a press on the grip still auditions the note.
- The gesture commits exactly one undo snapshot (`edited()` once) on release,
  and never touches pitch, time, or velocity.
- No engine or session changes needed: the score builder already consumes
  `durationBeats` for note length and ending.
- UI harness (extended to 57 checks): a grip press resizes 0.5 → 1.0 beats
  (start, pitch, and velocity untouched), the note body grows past its old
  right edge in the rendered pixels (221 changed px in the old-empty strip),
  exactly one commit fires, and dragging the grip past the start clamps the
  duration to 0.1.

## Phase 8 — Multi-select, group edit, rubber band (done)

- Left-drag on empty canvas draws a rubber-band marquee that live-selects every
  note it crosses (a dashed green box is painted while dragging). Selection
  never auditions and never commits.
- Dragging any note that is part of a multi-selection moves the whole group by
  the same snapped time/pitch delta (clamped at the horizon and pitch range),
  auditioning the anchor note; releasing commits exactly one undo snapshot.
  Del/Backspace deletes the entire selection in a single undo step. Velocity
  and resize grips stay single-note (the note under the wheel/grip becomes the
  sole selection).
- All selected notes draw their border highlight; a plain click on an unselected
  note behaves exactly as before (single-note drag).
- No engine or session changes. UI harness (70 checks, +13): a marquee never
  auditions; group drag shifts both notes (+1 beat/+1 pitch each) touching
  neither the outsider note's time nor the group's velocities, with exactly one
  commit; group Del removes both selected notes in one commit.

## Phase 8 — Copy / paste / duplicate (done)

- `Ctrl+C` copies the selection (starts stored relative to the selection's
  earliest note); `Ctrl+V` inserts the copies selected, anchored at the visible
  playhead when one is shown, otherwise one grid step to the right of the copy
  origin; `Ctrl+D` duplicates the selection one step right of itself.
- Copying never auditions or commits; each paste/duplicate inserts the whole
  clipboard under a single undo snapshot and leaves the new notes selected
  (ready to move as a group). Pitches, velocities, and durations are
  preserved exactly.
- No engine or session changes. UI harness (96 checks, +26): the two-note copy
  pastes with relative spacing intact at 1.25/5.25 (step-anchored), duplicates
  at 1.5/5.5, and a second paste anchors at a visible playhead (7.0/11.0), with
  exactly one commit per paste and pitch/velocity/duration preserved throughout.

## Phase 8 — Quantize (done)

- A toolbar `Quantize` button (and the `Q` key) snaps the selection's start
  beats to the fixed 1/16 grid; with nothing selected it snaps every note.
  Quantize is independent of the snap toggle (which only gates manual gestures),
  never auditions or inserts, and commits exactly one undo snapshot when
  anything moved.
- No engine or session changes. UI harness (107 checks, +11): the key and the
  button each quantize a selection (3.7→3.75, 9.4→9.5) with one commit, an
  unselected notes run also works, off-grid neighbors are untouched, and no
  audition fires during quantize.

## Phase 8 — Loop-region playback (done)

- The transport now supports a loop region end-to-end: a `Loop` control kind
  (`AudioEngine::setLoop`) carries enabled + start/end **beats** onto the audio
  thread, which converts them to transport frames and drives the existing
  `Transport::setLooping`/`setLoopRange` wrap in `advance()`.
- The loop region is persisted in the Session (`loopMode` / `loopStartBeats` /
  `loopEndBeats`) and flows through the same undo/redo, new/open/save paths as
  tempo.
- UI: a `LOOP` button on the transport bar toggles the engine region on/off; the
  piano-roll ruler shades the region in green and lets you redraw it by dragging
  across the ruler (snapped, one undo step).
- No change to the MIDI player: on wrap the block start lands near `loopStart`,
  the player's rewind detection restarts the scan, and the deterministic
  rewind-render test still holds.
- Engine suite 54009 checks / 0 failures (+6: loop serialization round-trip,
  disabled-mode overrides, disabled transport does not wrap). UI harness (115
  checks, +8): a ruler drag sets the session to [2,6] with one edit, the ruler
  paints the shaded region (1560 loop pixels vs 0 outside), and the region
  survives a save/load round-trip.

## Phase 8+ — Audio clips: record, timeline, playback, import, export (done)

- **Session** gains audio clips: a `Track` can carry an ordered list of
  `AudioClip`s (name, source file, start/duration in beats, sample rate,
  channels), serialized additively in v2 documents (v1 files still load).
- **WAV I/O** (`audio/WavFile.{h,cpp}`, libsndfile): `readAudioFile` (format,
  frames, rate, channels, floats) and `writeWavSamples` (PCM16 or float).
- **`dsp::ClipPlayer`**: a sidechain player that renders each clip's
  sample-accurately on the audio thread from a published triple-buffer
  snapshot (`AudioEngine::setClips(clips, storage)`), resampled to the output
  rate, amplitude-gated by the track, phase-continuous across block edges and
  rewind/loop. (Fixed a real bug here: the right channel was written at
  `outR[n * stride + 1]` with `outR` already channel-offset, leaving R silent
  and smearing the tone into the wrong slot.)
- **`audio/ClipScore.h`**: `buildClipScore` sorts active unmuted clips by
  start beat and bounds them against a time horizon, mirroring the MIDI score.
- **AudioEngine record**: capture runs only while the transport plays;
  `startRecord`/`stopRecord`/`takeRecordedSamples` bound the buffer at 5
  minutes; recorded samples are retrievable for the UI to persist as WAV. The
  loop-path rendering harness writes a clip to WAV over a 16-beat span and
  verifies per-beat RMS (one tone could beat-by-beat, the transient-injecting
  clip peak scans, silence after the clip).
- **UI**: the piano roll grows a 28 px audio-clip lane above the note grid
  that paints waveform strips (peak-binned per 16th, cached by content
  signature), selects/drags/un-snaps a clip across the timeline (one undo
  step), and deletes it with Del/Backspace or right-click. A record button on
  the transport bar arms recording from the playhead and, when stopped, writes
  the take to `<project>/audio/clip_NNN_<epoch>.wav` and drops a clip on the
  active track. An "Import Audio…" toolbar button reads WAV/FLAC/OGG/AIFF,
  computes duration from the tempo, and adds the clip (staged into the project
  folder when one is open). Every route into the session (edit, undo/redo,
  new/open) rebuilds and republishes the clip score, so clips follow the
  transport live.
- A heap-corruption bug found while testing this landed in the unrecord test
  itself (`out(1024)` < 2×4800-frame chunk writes), not the engine; the scratch
  buffer was corrected to `kChunk * 2`.
- Engine suite green: 54092 checks, 0 failures. UI harness (129 checks, +14)
  now drives the clip lane headlessly: a generated tone clip paints its lane
  strip (614 clip pixels, zero below), drag commits startBeats 2→4 under one
  undo snapshot with duration untouched, Del removes it with one commit, and
  clips survive a save/load round-trip.

## Phase 8+ — Beyond

- VST3/LV2 plugin hosting, buses and routing, recording, rendering, GPU
  acceleration for the UI when it pays off.

## Status conventions

- `done` — builds, runs, and tests pass.
- `current` — being implemented in this session.
- Anything "planned" stays a bullet above; nothing is marked done until it is
  actually verified.