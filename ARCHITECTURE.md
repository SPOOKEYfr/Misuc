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
  (`kSchemaVersion = 1`): schemaVersion, app identity, metadata, transport.

## UI (`ui/`)

The UI is a Qt 6 Widgets application (`misuc_app`) with a custom-painted
premium dark theme. Layout comprises a top transport bar, sidebars, status
area, and content pages. The first real content page is an **Audio Settings**
page that drives the live engine: it lists devices (output and input) from
`PipeWireBackend::enumerateDevices`, offers sample-rate/buffer choices, shows
latency, toggles the 440 Hz test tone and the groove beat, controls master and
monitor gain, and shows live output/input meters with clip LEDs.

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
