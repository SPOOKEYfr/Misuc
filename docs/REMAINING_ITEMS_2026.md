# Remaining items after the 19-item roadmap

Status as of Sep 2026. The 19-item implementation roadmap
(`docs/IMPL_ROADMAP_2026.md`, items 01–19) is **COMPLETE**. This report
collects everything that remains *outside* that roadmap: deliberate
deferrals, documented partials, known limitations, and housekeeping. It does
**not** re-open finished components.

Every claim below is backed by a `file:line` reference so the next pass can
start from ground truth. Nothing here is tested by the current suite as a
live failure — these are **honest gaps**, not regressions.

---

## Summary

| # | Item | Kind | Status |
|---|------|------|--------|
| 1 | Headroom ceiling not wired to any UI/control | Partial feature | Open |
| 2 | True-peak meter is a midpoint estimate, master-only | Partial feature | Open |
| 3 | Per-track input-latency offsets | Known limitation | Out of scope (documented) |
| 4 | Monitor-playthrough RT delay line | Known limitation | Out of scope (documented) |
| 5 | VST3 plugin hosting | Deferred | User directive ("not until core stable") |
| 6 | FL sampler-channel note hits not rendered | Partial (honest) | Documented |
| 7 | Metronome sound-design improvement idea | Idea | User request, unimplemented |
| 8 | Versioning / git hygiene | Housekeeping | Open (repo rule blocks commits) |
| 9 | QA environment flakes | Non-code | Pre-existing, environmental |

(resolved Sep 14 under the UI-hardening pass: the Qt `.qrc` was never embedded,
the idle engine+transport repaint burn, the browser/grid repaint storms, and
the live-PipeWire 90 ms audibility flake are all fixed — see below.)

---

## 1. Headroom ceiling has no UI/control

- **Evidence:** `MISUC_CURRENT_STATUS.txt:13` marks this PARTIAL. The DSP
  already has `setCeiling(float)` — `engine/include/misuc/engine/dsp/Dsp.h:284-291`
  (clamped to `[1e-3, 1]`, default `0.891f` = −1 dBFS at `Dsp.h:317`) — but no
  UI/control or engine command exposes it.
- **Gap:** ceiling is effectively fixed at −1 dBFS until a control surfaces it.
- **Suggested next step:** an Audio Settings slider bound to a new engine control
  (mirroring `SetCaptureOffset`'s control-channel pattern), defaulting unchanged
  to −1 dBFS; engine test + QA hint registration.

## 2. True-peak meter is a midpoint estimate, master-only

- **Evidence:** `MISUC_CURRENT_STATUS.txt:14` marks this PARTIAL.
  `interpolatedPeakChannel` (`Dsp.h:106-135`) evaluates a Catmull-Rom cubic at
  `u = 1/2` only, falling back to the block peak; it is applied to **master
  meters only** — per-track and per-bus meters use plain peak/RMS.
- **Gap:** not 4× oversampled true-peak; per-track/bus not interpolated. The old
  status text that claimed "4x oversampled per-track/bus/master" was corrected in
  the same line.
- **Suggested next step:** optionally run a proper 4× oversampled estimator and
  apply it per-track/bus, or explicitly keep the cheap estimate and update docs.

## 3. Per-track input-latency offsets

- **Evidence:** `docs/COMPONENT_STATUS_MATRIX_2026.md:235`, `MISUC_CURRENT_STATUS.txt:181`,
  `docs/ARCHITECTURE_PRIORITY_PLAN_2026.md:260`.
- **Gap:** latency compensation applies one uniform global latch shift
  (`SetCaptureOffset`); per-track JSON offsets for devices with differing
  latencies remain out of scope.
- **Suggested next step:** treat as a deliberate non-goal until multi-device I/O
  is a real requirement; re-document if the device-vector model changes.

## 4. Monitor-playthrough RT delay line

- **Evidence:** same three references as #3.
- **Gap:** monitoring passes input through without a real-time delay line, so
  monitor alignment is not compensated to the same precision as recording.
- **Suggested next step:** fold into the same latency pass as #3 (the global
  capture buffer is the honest first pass).

## 5. VST3 plugin hosting — deferred by directive

- **Evidence:** `docs/COMPONENT_STATUS_MATRIX_2026.md:67`,
  `docs/ARCHITECTURE_PRIORITY_PLAN_2026.md:125` — "No VST3 (user directive: not
  until core stable)".
- **Gap:** LV2 hosting + built-in EQ/compressor cover the FX rack; VST3 is
  intentionally absent.
- **Suggested next step:** revisit only when the core is declared stable.

## 6. FL sampler-channel note hits are preserved, not rendered

- **Evidence:** `MISUC_CURRENT_STATUS.txt:204` (PARTIAL, documented).
- **Gap:** FLP import keeps sampler-channel rows (auditionable, warning issued),
  but their note hits are not rendered as pitched regions (Misuc regions are
  pitched).
- **Suggested next step:** a future waveform/sampler region type, or leave the
  documented honesty in place.

## 7. Metronome pattern-improvement idea (user request)

- **Evidence:** `docs/COMPONENT_STATUS_MATRIX_2026.md:225` — "None remaining
  beyond the documented pattern-improvement idea (user request). Click is
  monitoring-only by design."
- **Gap:** the click is a fixed percussive design; a pattern idea was raised and
  not implemented.
- **Suggested next step:** confirm scope with the user before spending time.

## 8. Versioning / git hygiene

- **Evidence:** `MISUC_DEVELOPMENT_LOG.txt:101-110`; `VERSION` still 0.1.0 and
  uncommitted (repo rule: no commits unless asked); stale 0.1.0 packages/bins in
  `build/`, `build-debug/`, `build-asan/`, `build-engine/`; remaining 0.1.0 quotes
  at `MISUC_SPEC83_FINAL_REPORT.md:58,174` and `docs/COMPONENT_STATUS_MATRIX_2026.md:253`
  (the old `MISUC_CURRENT_STATUS.txt:149` quote was already fixed by the
  55242-test doc update).
- **Suggested next step:** a single version-consistency pass the next time a
  commit is explicitly requested.

## 9. QA environment flakes (non-code)

- **Evidence:** `docs/COMPONENT_STATUS_MATRIX_2026.md:251` — "ASan clean except
  the pre-existing test_audio live-PipeWire env flake (and the unrelated
  PipeWire 8192 UBSan warning)". Observed again Sep 2026: the browser's filesystem
  scan (`ui/src/browser/Browser.cpp:638-688`) can stall for tens of seconds on a
  large shared NTFS partition (`/media/oibhupi/Windows/...`, 148 GB, ~7 k loose
  audio files), making some GUI QA runs time-dependent. The separate
  `midi_playback_lifecycle` audible check (`tests/test_audio.cpp:82-91`) also
  flaked because its 90 ms wait sat inside one device quantum (~170 ms here);
  the wait is now 600 ms (`test_audio.cpp:83-84`).
- **Gap:** not a product bug; harness robustness. (Fixed the user-facing impact
  Sep 2026: startup no longer scans it — browser opens on "Current Project" and
  waveform thumbnails run on a capped 2-worker pool.)
- **Suggested next step:** allow overriding the browser's scan roots in the QA
  harness, and/or wrap the GUI scenarios in a mount-aware sandbox.

---

## Clarification: items that are NOT remaining

- The old PARTIAL markers in `MISUC_DEVELOPMENT_LOG.txt:128-160` are the
  bootstrap-era component table; every component it rated PARTIAL was completed
  by a later component (01–19 COMPLETE).
- `ROADMAP.md` Phases 0–18 (plus the "Beyond" entries) are all marked `done`.
- `misuc_tests`: 56,380 checks, 0 failures; all five build targets build clean.

### UI-hardening batch (Sep 14) — resolved, do NOT reopen

These were discovered while fixing the user's "not responding / flickering /
empty user guide / snap / scrolling / move-between-tracks" report and are now
shipped (build + tests/QA + guide + this doc in one pass):

- **Qt resources were never embedded.** `ui/CMakeLists.txt:67` used
  `qt_add_resources` on Qt 6.4/CMake 3.28, which silently compiled nothing — the
  binary had no `qt_resource` frames, so `:/help/user-guide.txt`, the window icon
  and the about image were empty/missing (the "empty user guide" report). Now an
  explicit `rcc` custom command + `target_sources` (`ui/CMakeLists.txt:67-86`)
  with full dependency tracking; verified by grepping the installed binary for
  the guide text and resource frames.
- **Idle engine burn.** `AudioEngine::process` ran the whole per-track mix,
  bus/send/FX routing and meters on every block in every state, synthesising
  silence (~50% of one core idle with the demo open). A `idleSilent` park
  (`AudioEngine.cpp:1257-1265`) skips the region when stopped with no score/clip
  slot, no input recording and no audible audition (new
  `MidiPlayer::hasAudiblePreviews`, `Midi.h`) — the master/limiter/meter tail
  still runs. Idle audio thread drops ~50% → ~20%.
- **Transport repaint storm.** `TransportBar::refresh()` called `update()`
  unconditionally from the 30 Hz poll, repainting the whole bar (~20% of one
  core, flicker). Now gates on actual state change (`TransportBar.cpp:81-104`).
- **Playlist/PianoRoll playhead gates.** `Playlist::feedPlayhead` and
  `PianoRoll::setPlayhead` previously repainted the full grid/editor every
  33 ms tick even while idle; both now early-out unless the caret/state changed
  and render only the caret columns while playing.
- **Playlist ergonomics:** snap-to-grid now defaults ON (`playlist/Playlist.cpp`
  `snapOn_ = true`), the grid scrolls with the keyboard on both axes
  (arrows/Page/Home/End/Ctrl+Home), and patterns/clips move between tracks via
  Ctrl+Up/Ctrl+Down or right-click "Move to Track Above/Below".
- **User guide rewritten** into a real tutorial covering all shipped features
  (`ui/resources/help/user-guide.txt`, embedded via the fixed `.qrc`).

## Protocol reminder for the next pass

Per the master protocol, any of the items above that move to implementation
must land with tests + QA + truthful doc updates in the same pass, and none may
ship as a knowingly-failing test.