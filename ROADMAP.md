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

## Phase 3 — Real audio output & input

- AudioEngine playback of generated audio (test tone already exists; extend to
  render from the transport/metronome).
- Capture path: input device → engine → metering, input monitor.
- Round-trip latency verification with an audible + measurable method.

## Phase 4 — Project workflow

- File menu: New / Open / Save / Save As for `.misuc`.
- Session persistence via JSON; undo stack foundation.

## Phase 5 — Core UI/peripherals

- Arrangement/mixer themes, piano roll rendering, timeline, MIDI note entry.
- Undo/redo for editing, snap settings, zoom.

## Phase 6+ — Beyond

- VST3/LV2 plugin hosting, buses and routing, recording, rendering, GPU
  acceleration for the UI when it pays off.

## Status conventions

- `done` — builds, runs, and tests pass.
- `current` — being implemented in this session.
- Anything "planned" stays a bullet above; nothing is marked done until it is
  actually verified.