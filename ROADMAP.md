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
- File→Export Audio… (Ctrl+E): the File menu gains an action that computes the
  project extent from notes and clips, breaks down / bounces the mixdown
  through `AudioEngine::renderToWav` (clip+note score), stops and restores the
  audio stream around the offline render, and writes a stereo WAV. Verified
  end-to-end off the MainWindow: injected note+clip, trigger the action, read
  back the WAV and check frames (8 beats → 192000 frames @120bpm), clip peak
  in-beat, note region, the beat-4..6 gap staying silent, and the transport
  still stopped. (Harness 142 checks.)
- Clip editing on the lane: right-edge grip stretches `durationBeats`
  (left edge + clipOffset fixed), left-edge grip trims-in (right edge fixed,
  pushes `clipOffsetBeats` deeper into the source), Ctrl+D duplicates the
  selected clip one snap step right with a fresh id, and the wheel over a clip
  adjusts clip `gainDb` (±1 dB) with the same debounced undo snapshot as note
  velocity. Edge grips get a resize cursor and hi-lite handles. (Harness 166
  checks.)

## Phase 9 — Real mixer: engine + panel (done)

- **Engine** (`AudioEngine`, `audio/MidiScore.h`, `audio/ClipScore.h`,
  `dsp/Midi.{h,cpp}`, `dsp/ClipPlayer.h`): every track now flows through its
  own mixer stage on the audio thread. Notes and clips carry the id of the
  track they belong to; the scorers no longer bake track gain or skip muted
  tracks — they emit raw pitches/clips plus a per-track amplitude. The engine
  applies gain/pan/mute/solo per track at render time from a lock-free
  published `publishMixer(std::vector<MixerTrackParam>)` (gainDb, pan, mute,
  solo), with sample-accurate smoothed faders, a constant-power `dsp::panLaw`
  (centre = 0.707 per channel), and `anySolo` gating: a track is audible iff
  `!mute && (!anySolo || solo)`.
- Score/clip slots store per-track vectors partitioned on the publisher thread;
  `setClips` moves the sample storage into the slot and rebases pointers
  (identity, since a `std::vector` move preserves the allocation address).
  `kMaxTrackCount = 32`; `process()` renders the per-track section in 8192-frame
  chunks so the large offline/export blocks stay allocation-free.
- **Per-track output metering**: the meter measures post-gain (fader + pan),
  pre-mute/solo-gated signal, exposed as non-blocking atomics
  (`trackMeterPeakL/R`, `trackMeterRmsL/R`, `trackMeterClipped`). The master bus
  and meters are unchanged; `renderToWav` shares the live `process()` path so
  exports honor the mixer exactly.
- **UI mixer panel** (`ui/src/mixer/`): a real Mixer page with one `ChannelStrip`
  per track — color dot, pan `Knob` (−100…+100, drag/wheel/double-click-centre),
  M/S buttons, a vertical stereo `VerticalMeter` (RMS fill, peak-hold cap, clip
  LED), and a dB `Fader` (−60…+12, wheel fine-tune, double-click → 0 dB) plus a
  gain readout — a dedicated Master strip (fader + stereo meter) on the right.
  Strips mirror the Session; edits report up through `edited()` into the same
  undo stack as every other edit, and `syncToEngine()` re-publishes the mixer
  params whenever the session changes (edit, undo/redo, open, new). `pollMeters()`
  runs on the existing 30 Hz UI tick from the engine atomics. Track selection
  is mirrored both ways between the editor list and the strips.
- Theme center landed under the same phase boundary (see Phase 10).
- All verification green: unit `tests/test_mixer.cpp` (+5 cases: per-track gain
  independence, hard-pan L/centre/R, solo + multi-solo windows, mute gates but
  keeps its meter, export honors the mixer), full engine suite 54127 checks / 0
  failures; the UI harness grew to 197 checks (mixer strips mirror the session,
  strip edits land in the session and are undoable, strip activation mirrors
  selection into the editor list, master fader + readout, meter polls, theme
  preset/custom-accent/`.misuctheme` round-trip, glass toggle notification,
  nav-logo pixmap).

## Phase 10 — Central theme engine + appearance page (done)

- `theme::k*` tokens are now mutable inline globals owned by
  `ui/src/theme/ThemeEngine.{h,cpp}`; every existing call site is unchanged.
- Eight factory presets (Misuc Graphite, Midnight Blue, Violet Studio, Electric
  Cyan, Crimson Studio, Emerald Studio, Amber Studio, OLED Black); a custom
  accent sets the preset name to "Custom" and recomputes the derived tokens
  (`accentSoft = blend(panel, accent, 48)`) so buttons, sliders and the mixer
  stay legible. An optional glass layer with an opaque fallback
  (`glassPanelStyle`) drives the translucent mixer strips.
- `apply(QApplication&)` sets the Fusion palette + app stylesheet and emits
  `themeChanged()` (suppressed on the first apply). Choices persist via
  `QSettings` (`theme/preset`, `theme/accent`); `.misuctheme` files round-trip
  as indented JSON for sharing.
- New **Appearance** page (`ui/src/theme/ThemePage.{h,cpp}`) in the nav rail:
  preset combo, accent color picker with live swatch, glass toggle, and
  Save/Load `.misuctheme`, with a live preview panel — all edits re-apply and
  persist instantly.

## Phase 11 — Branding + Linux desktop integration (done)

- Vector logo (`ui/src/branding/Logo.{h,cpp}`): a gold MIDI note whose stem
  blooms into a coral flame, painted with `QPainter`; the nav-rail brand mark
  and window icon draw from the **active theme accent** at runtime.
- `tools/rendericons.cpp` regenerates the on-disk hicolor icon set (16–256 px)
  plus the bundled app icon (`ui/resources.qrc` → `:/icons/misuc-256.png`) —
  the app loads the real generated PNGs.
- Desktop integration: `assets/desktop/misuc.desktop` (validated with
  `desktop-file-validate`) + `cmake --install` stages the binary, the entry,
  and the hicolor icon set into `share/…` (verified with a `DESTDIR` strip).
- Harness checks the nav logo pixmap and the full theme round-trip; app smoke,
  engine suite and harness all green.

## Phase 12 — Portable projects & record-through verification (done)

- **Portable asset paths (Item #5)**: `Session` gains
  `relativizeAssets(baseDir)` / `resolveAssets(baseDir)` — every clip path that
  lives under the project folder is converted between absolute and
  project-relative form, so a saved `.misuc` stays portable next to its
  `<folder>/audio/` assets. `MainWindow::writeProject` serializes a
  relativized *copy* (the live session keeps absolute paths, so playback is
  unaffected after a save), and `openProject` resolves relative references
  back to absolute paths immediately after loading (before the clip score is
  rebuilt). External assets outside the project folder keep their absolute
  references. Unit tests in `tests/test_json.cpp` (+6): absolute → relative →
  absolute round-trip, external paths stay absolute, and `~`/relative
  references survive a resolve unchanged. The UI harness (Phase 14, now 207
  checks) does an end-to-end Save → inspect the document on disk → `openProject`
  round-trip with a real asset staged in `audio/`, asserting the saved JSON
  holds `audio/tone.wav` and the reloaded session resolves it back to an
  absolute path. `writeProject`/`openProject` are exposed as public test hooks
  next to the existing `session()`/`setExportPathOverride`.
- **Record playthrough integration (Item #4)**: `tests/test_record.cpp` (+2
  cases) drives an offline `AudioEngine::process()` with a published score and
  a deterministic sine input while the transport plays. It proves the take
  captured during playback equals the fed input sample-for-sample (< 1e-4) and
  is mono at the project rate — and that the same input prints into the
  monitored output where the loop plays (loop + input louder than the loop
  alone) and appears on its own past the loop while the transport runs on
  (real playthrough, monitor buses at the default 0 dB). A second case confirms
  a stopped transport captures nothing (`takeRecordedSamples` stays empty), the
  behavior the count-in UI relies on.
- All verification green: engine suite **54145 checks / 0 failures** (+6 from
  the asset-path unit tests, +12 from the record-playthrough cases), the UI
  harness runs **207 checks, ALL OK**, the app smoke-tests clean, and the
  build is warning-clean.

## Phase 13 — UI foundation redesign: modular DAW shell (done)

Per the master product spec §58 Phase 1 (UI foundation). Existing audio
foundation untouched (engine suite still 54145 checks / 0 failures).

- **Global header** (`HeaderBar`) replacing the nav rail: brand cluster
  (note-flame mark + MISUC wordmark + live project name) left, transport
  centred, live status cluster right — real **app CPU% and RAM** from
  `/proc` (`/proc/self/task/*/stat` jiffies, `/proc/self/status` VmRSS),
  sample rate, buffer + output latency, xruns, transport state, and two master
  mini meters with clip flags.
- **Tabbed central workspace** (`QTabWidget`): Project | Editor | Audio |
  Appearance; the Mixer moved to a closable/floatable/movable **QDockWidget**
  (bottom by default). Same widget creation order so the editor's track list
  stays the window's first `QListWidget` (embedded-library contract intact).
- **Hint system** (`HintSystem`): persistent left **HintBar** in the status
  strip (name — description, live `[value]`, shortcut chip) driven by a
  hover-event registry (`HintManager`) with a default hint; refreshed on the
  UI poll tick. HintBar is registered over every major page.
- **Theme engine expansion**: ~24 new semantic tokens (layered charcoal-glass
  surfaces, text hierarchy, interactive states, chrome, editor + meter tokens);
  QSS for dock/tab/toolbar/status/menu chrome; hard-coded hover/pressed colors
  removed. Editor-neutral tokens keep stock values so note/clip rendering is
  pixel-identical and the harness pixel checks stay green.
- **Vector icon set** (`branding/Icons`): metronome, loop, count-in, settings,
  audio, question glyphs painted from theme colors (no random Unicode).
- **Metronome/count-in now real**: the transport click button arms
  `engine_.setMetronome(on, countInBeats)` (previously the signal was never
  consumed). Default off so playback/export stay click-free.
- **Workspace menu + layout persistence**: Default / Mixing / Piano Roll /
  Recording presets (real dock visibility + tab switches), F7 → Piano Roll,
  F9 → toggle Mixer, Space → play/stop; dock layout, active tab, and window
  size persist to QSettings.
- **§57 audit fixes landed**: master-fader now persists (`Session` gains
  additive `transport.masterGainDb`, v3-compatible; loaded with 0 dB default,
  applied to the engine on every sync), PianoRoll's six hard-coded paint
  colors tokenized (pixel-identical), and the first mixer strip follows the
  theme accent instead of a fixed literal.
- Planning artifact: `MISUC_UI_IMPLEMENTATION_PLAN.txt` (current + proposed
  architecture, files changing/stable, migration, theme/workspace/plugin
  architecture, packaging strategy, test strategy, §58 phase lanes).
- Verification: engine **54145 / 0**, harness **213 ALL OK**, smoke rc=124
  (alive), desktop-file-validate rc=0.

## Phase 14 — Workspaces: Playlist, Channel Rack, Browser, Inspector (done)

Per the master product spec §58: modifiable workspaces on top of the
unchanged engine core (engine suite still 54145 checks / 0 failures).
Workstations mutate the Session only via the public `tracks()` reference; no
engine code changed.

- **WorkspaceManager**: owns the central `QTabWidget` and every `QDockWidget`
  in one place. Central screens (Playlist, Piano Roll, Project, Audio,
  Appearance) and docks (Browser left, Mixer bottom, Inspector + Channel Rack
  right) are registered as one managed set with `isVisible()`,
  `showWorkspace()` / `toggleWorkspace()`, six presets (Default / Playlist /
  Piano Roll / Channel Rack / Mixing / Recording), and full QSettings
  persistence of dock states, active central tab, and chosen preset.
- **Playlist** (`Playlist` + `PlaylistGrid`): real arrangement view bound to
  the Session — per-track lanes with mute/solo dots, a time ruler, a violet
  playhead fed from the engine, real waveform peaks for audio clips
  (`<sndfile.h>`, windowed 256×256 sampling, cached) that play back through
  the engine, MIDI regions derived from track notes, clip drag/move/resize
  with beat snapping, split-at-playhead, duplicate, delete, rename via a
  context menu, drag-and-drop import from the Browser, Import Audio, and
  Select / Slice / Snap toolbar tools. Clip selection is mirrored to the
  Editor, Mixer, and the Inspector; undo/redo hook into the editor's session
  stack.
- **Channel Rack**: step sequencer as a QTableView + QStandardItemModel +
  custom delegate — 16 gated steps per channel (violet active cells), mute /
  solo / volume / pan per channel, persisted per-rack snapshots to QSettings
  (base64 step masks), and a starter set of Kick / Snare / Hi-Hat / Bass /
  Sampler channels.
  - **Phase 14.3**: four 16-step pattern banks (numbered selector) + a 0-66%
    swing slider, both persisted (legacy single-bank blobs migrate); step
    lanes gain a divider every four steps. Audio pipeline remains frozen, so
    swing/pattern are sequencer-model controls documented as pending the
    unfrozen playback path — never faked as DSP.
- **Browser** (left dock): category tree (All Samples, Current Project,
  Instruments, Audio Effects, MIDI, Plugins — plugin rows are informational
  only, no fake DSP), recursive sample scan (Music + home music dirs, capped
  at 4000 files), search filter, real preview via `audio::readAudioFile`
  (decoded waveform + metadata, decoded on demand), and drag-and-drop / Insert
  that adds samples straight into the Playlist.
- **Inspector** (right dock): context panel that mirrors the current
  selection — workspace intro, track, audio clip, MIDI region, or note
  properties — driven live by Playlist selection. Read-only by design; edits
  happen in their owning workspaces.
- **Phase 14.2 — visual redesign + live Inspector (done):**
  - Matte-glass chrome: `theme::kChrome`/`kChromeRaised`/`kChromeBorder`
    tokens (Theme.h) + ThemeEngine QSS rewrite (explicit high→low placeholder
    substitution; zero `QString::arg` warnings). Global transparent `QWidget`
    background with explicit per-view QSS.
  - Icon-only chrome: Playlist toolbar (Select/Slice/Snap/Add + ZoomIn + zoom
    slider), Browser Preview/Insert, Channel Rack header (Preview/Add/
    Duplicate/Remove/Reset).
  - Fully developed hover HintBar (`HintManager::registerTool` /
    `registerCustom`) across Playlist, Channel Rack, Browser, HeaderBar strip +
    status labels, and the custom TransportBar (position-aware resolver, live
    position readout via QCursor::pos() over painted button rects).
  - **Inspector is now a live Session-backed editor**: editable track
    name/mute/solo/gain/pan and clip name/gain/trim push mutations straight
    into the Session (undo-tracked, engine score refreshed). Verified by
    `qatool inspect`, 5/5.
  - QA tooling recreated at /tmp/opencode (qatool: `audit` / `snap` / `behave`
    / `inspect`; verify_pixels.py PNG verifier). Layout auditor: **0 issues /
    294 widgets** at 1280x800, 1440x900, 1920x1080; behave 9/9; snapshots
    pixel-verified (charcoal backdrop, violet-accent chrome band).
  - Packaging closed: 512px hicolor + bundled icons shipped; install staged +
    `.deb` regenerated and contents-verified.
- **Header strip → workshop wiring**: the global header's workspace strip now
  communicates with the WorkspaceManager (toggle ↔ active highlight), the
  snap/mode buttons drive the Playlist, F5/F6/F7/F8/F9 + Ctrl+Shift+I toggle
  every workspace, and the transport clock stays the single playhead source.
- **Menus + commands**: restructured menus (File / Edit / Add / Patterns /
  View / Options / Tools / Help) where every item either maps to a
  `MisucCommandRegistry` command (workspaces, transport, metronome, snap,
  song/pattern mode) or is wired to real pages; "Export &Audio..." and
  "&About Misuc..." preserved.
- **Linux packaging**: CPack DEB (`build/misuc_0.1.0-1_amd64.deb`) installing
  the app, the hicolor icon set, and the validated `.desktop` entry
  (`desktop-file-validate` rc=0) with `StartupWMClass`.
- Verification: engine **54145 / 0**, harness **ALL OK** (full workspace
  suite), offscreen smoke rc=124 (alive), pixel-verified renders of the
  default / piano-roll / focus workspaces (theme tokens present, waveforms and
  clip bodies drawn, violet playhead rendered).

## Phase 8+ — Beyond

- VST3/LV2 plugin hosting, buses and routing, recording, rendering, GPU
  acceleration for the UI when it pays off. Next milestones for the modular
  shell per §58: desktop branding + .deb (P2), Browser (P3), Channel Rack
  (P4), Playlist (P5).

## Status conventions

- `done` — builds, runs, and tests pass.
- `current` — being implemented in this session.
- Anything "planned" stays a bullet above; nothing is marked done until it is
  actually verified.