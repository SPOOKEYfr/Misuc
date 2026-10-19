# Misuc — Architecture & Phased Implementation Plan (Sep 2026)

## 1. Architecture map today

Layers, top to bottom:

```
UI (Qt widgets)  MainWindow / Playlist / PianoRoll / ChannelRack / MixerPage
   |              Browser / Inspector / HintBar / HeaderBar / Theme / Workspace
   v
Session layer    misuc::project::Session  (single source of truth; JSON v2 .misuc)
   |              save/load, autosave, undo snapshots, contentEndBeats
   v
Publish layer    buildMidiScore / buildClipScore / publishMixer(+buses) /
                 publishAutomation / publishFx  -> lock-free ControlMessage queues
   v
Engine (audio)   AudioEngine::process()  (real-time; never blocks/allocates)
                 Transport/Metronome/Groove | per-track gain*pan*automation*FX
                 | bus gain | master gain → output (+ meters, record capture)
   v
Backend          PipeWireBackend (AudioBackend iface) — live stream
                 WavFile (libsndfile) — offline renderToWav == live path
```

Threading: UI thread owns Session; audio thread owns transport + DSP. Hand-off
via `SPSCRing` triple-buffered publishes. This is sound. The changes below keep
it.

## 2. Dependency graph

```
Phase A  audio foundation (no UI dep)  [DONE Sep 2026] ┐
Phase B  project model v3 (needs A for export)  │  ── every later phase builds on A+B
Phase C  piano roll (model: B)                 ↑
Phase D  channel rack in-session (model: B, engine: A) ── uses C note lane ideas
Phase E  playlist tools (model: B)             ── uses D patterns
Phase F  mixer+EQ+routing (engine: A)          ── adds engine FX/meter lanes
Phase G  recording (engine: A)          [DONE]   ── uses F monitoring
Phase H  export FLAC (WavFile)                 ── uses A master path  [DONE]
Phase I  browser SOUNDS (model: B)  [DONE Sep 2026]  ── independent, uses F preview
Phase J  UX/hints/icons/commands/inspector     ── thin over C–I
Phase K  workspace/docking/flicker             ── UI-only
Phase L  automation lanes (engine: A)          ── extends model: B
Phase M  QA/package/install verification        ── everything
Phase N  fx control-port automation             ── engine: A, model: B
Phase O  per-block fx control smoothing         ── engine: A
```

## 3. Master clipping root cause & Phase A design (the first technical fix)

Measured evidence in `docs/COMPONENT_STATUS_MATRIX_2026.md` (§ "Root cause").
3+ coherent hot parts hard-clip the master; no limiter/softclip exists.

Phase A acceptance ("no surprise clipping"):
- Add a real **master bus limiting/soft-clip stage** after the master gain
  (smooth, zero-latency-tolerant; decision below), so sums past 0 dBFS are
  contained gracefully instead of squaring off.
- Keep live + export identical (both go through the same stage).
- Honest **true-peak meters** per master/track/bus; clipping indicator that
  flags the actual clipped samples.
- `misuc_headroom` regression: a 12-hot-track stress render must never exceed
  the chosen ceiling and must show ZERO hard-clipped samples.
- Headroom policy defaults (decision below): professional default e.g. track
  faders start at 0 dB but master limiter holds the ceiling, OR automatic
  master trim. Choose with the user before implementing.

## 4. Phases A–S (deliverables + definition of done)

Every component: report with the required format and exit criteria
(COMPONENT / STATUS / REFERENCE RESEARCH / FILES CHANGED /
ARCHITECTURAL CHANGES / ENGINE CHANGES / SESSION-MODEL CHANGES / UI CHANGES /
COMMANDS / USER WORKFLOW / AUTOMATED TESTS / MANUAL TEST / INSTALLED BUILD
TEST / KNOWN LIMITATIONS / DEPENDENCIES / NEXT COMPONENT). Never claim
complete while anything is fake/disconnected.

- **A — Audio foundation.** Master limiter/softclip + headroom policy + true
  peak meters + clip indicators + xrun/glitch handling; keep live==export.
  DoD: headroom stress green; no xrun glitch artefacts in a 10-min soak.
- **B — Project foundation.** `.misuc` v3 versioned schema; atomic write;
  robust autosave + crash recovery (pattern after the FLP autosave names);
  recent projects; clean session-new/open/save/save-as workflows.
  DoD: kill-the-app crash loses ≤ current autosave period; every control
  round-trips through the file byte-for-byte.
  *Phase B slice landed (Sep 2026): clean-room FLP structural reader (decision
  #2) — `misuc_flp` + `FlpReader`; oracle-verified; schema field vocabulary.*
  *Phase B slice 2 landed (Sep 2026): `.misuc` v3 — `"format":"misuc.song"`
  marker, `schemaVersion`, created/modified UTC stamps, forward-compat gates,
  legacy v1/v2 migration; `misuc::core::atomicWrite` (fsync+rename);
  autosave validate-before-promote rotation to `autosave.prev.misuc`;
  File→Open Recent (QSettings, max 12) + Clear Recent.*
  *Phase B slice 3 landed (Sep 2026): FLP→.misuc structural import —
  `FlpReader` notes decoded (24-byte FL20+ struct); `importSession()`
  maps channels→tracks, mixer inserts→buses, playlist items→MidiRegions
  (split by rack_channel); `misuc_flp --import x.flp --out y.misuc` CLI.
  Sampler-channel note hits skipped (honest); unassigned legacy notes get
  their own track; arrangement picked by item count. Oracle-validated on
  real files.*
- **C — Piano Roll.** Full MIDI 0–127; C1–C8 navigation; all chromatic notes;
  toolbar tools (pencil/select/erase/copy/quantize/velocity); key preview.
  DoD: build a 64-bar chromatic line, save/reload, playback identical; undo
  every edit.
- **D — Patterns / Channel Rack.** Move rack into the Session (persist);
  **fix DELETE/trash**; step+note lanes; swing; pattern-in-session so live/song
  mode is coherent; groove templates.
  DoD: save/reload a built rack; deleted channel disappears from playback and
  session; patterns survive reload.
  **DONE (Phase D, Sep 2026):** rack is a first-class Session element —
  `channelRack` JSON (name/mute/solo/volume/pan, 4 banks × 16 step lanes
  base64, activePattern, swing); keyboard DELETE/Backspace on the rack row;
  full undo/redo; programmatic step/mute/solo/volume sync before `edited()`.
  54600 engine checks + 310 QA checks green; ASan-clean. Drive-by: 3 Playlist
  lifetime bugs (region dup/slice UAF, automation-point insert iterator
  invalidation) + EditorPage item-widget ownership fixed under ASan soak.
- **E — Playlist — DONE.** Paint tool (B); move/resize/split/select/multi-select;
  per-channel clip lanes; zoom/snap; ruler click-seek; stop at real content end.
  DoD met: arrange a 100-clip song by painting; undo/redo every operation
  (hit as one stroke = one edit). Implementation: engine-centralized kit
  `rackRowFreq/rackRowPitch` + `materializePatternRegion` (single source for
  "Place Pattern" AND Paint); Paint button `plPaint`/Key B brushes the rack's
  active pattern into EMPTY bar cells over a held-drag; occupied cells never
  overwritten; release emits one edited(). QA firstfunk-paint (stroke +
  occupancy + one-step undo/redo) and firstfunk-paint-100 (100-bar painted
  song). 54624 engine checks + 325 QA checks; ASan-clean.
- **F — Mixer — DONE.** 125+ insert routing; per-insert fader/pan/mute/solo/meteor;
  bus/send routing; FX rack (LV2) with **built-in EQ** (no VST3 yet); routing
  matrix; insert-name/colour.
  DoD met: 125-insert session round-trips; routing graph renders sample-accurate
  (headroom tool on worst-case routing). Implementation: `kMaxTrackCount`
  32→128, `kMaxBusCount` 16→32; per-track/bus staging buffers moved off the
  stack to ctor-sized heap vectors (AudioEngine stays small for stack-allocated
  engine instances in tests); all fixed loops/meter arrays were already
  constant-driven so only the caps + scratch storage changed. Built-in EQ =
  `builtin:parametric-eq`: 4-band RBJ peaking (`Biquad::setPeaking`) + HP/LP
  pads, 14 ports, catalogued/slider-UI on every build, RT-safe (instantiated
  control-side, config immutable on the audio thread), URI + params survive the
  `.misuc` round-trip; FX publish/render/teardown dispatch on an FxKind
  discriminator instead of assuming lilv everywhere. Verified: `misuc_headroom
  worstcase` (125/125 inserts + 32/32 buses audible, 0 hard-clip samples, PASS);
  engine 55065 checks (incl. mixer_scale_125_inserts_render +
  fx_builtin_eq_*) + 331 QA checks (fx-ui drives the built-in EQ through real
  widgets); ASan-clean under `ulimit -s 65536`.
- **G — Recording — DONE.** True input capture + overdub, loop-record,
  metronome count-in, take list, monitoring; clip lands on the armed track at
  the transport position.
  DoD met: engine captures an input stream with a sample-accurate transport
  span tail-free up to the 5-min ceiling (`kMaxRecordFrames`); loop-wrap is ONE
  continuous take; count-in click beats never leak into the capture; the UI
  writes the WAV to project `audio/`, registers a Take, and places the clip on
  the armed track at the capture start; `.misuc` round-trip resolves the take's
  audio asset. Implementation: `RecordCapture` (samples/rate/channels/span +
  frames-dropped accounting) + `takeRecord` replacing the ad-hoc recorded-buffer
  copy; RT capture latches the transport frame at first write; metronome config
  now owns the count-in length (Play no longer overwrites it); Session gains
  `RecordTake` + per-track `recordArm` (JSON round-trip + id counter); mixer
  strip R button -> session arm + engine input-monitor gate
  (`setInputMonitor(anyRecordArmed())`); Inspector take list with comp-style
  place/remove. Verified: engine 55088 checks (incl. record_captures_*, 
  record_countin_excludes_click_beats, record_across_loop_wrap_*,
  record_monitor_off_*) + 363 QA checks (`record-ui` drives arm/monitor/
  transport-rec/take-list through real widgets); ASan-clean.
- **H — Export.** WAV (float+PCM16) + **FLAC**; range export; dither optional;
  export matches headroom policy (never silent-clips when master is clean).
  DoD: exported file decodes identically in a second reader; FLAC round-trips.
  **[DONE]** — `File > Export` (Ctrl+E) bounces through `AudioEngine::renderToFile`
  → `writeAudioFile` (libsndfile) in WAV float / WAV PCM16 / FLAC16 over Whole
  song / Loop / Selection, with TPDF dither on the fixed-word formats. Every
  container decodes identical through libsndfile's own reader (`test_export`
  bounds: FLAC ≤ 1 LSB, PCM16 ≤ 2 LSB, dithered ≤ 3 LSB; deterministic seeded
  dither, ±1 LSB DC-gate; warm FLAC-vs-float ≤ 2 LSB; [1,3) range == whole-song
  slice; 2.0-amp source held ≤ 0.892 across formats). QA `export-ui` (23 checks)
  drives the real menu path headlessly: whole/loop range, three formats,
  dither on/off word change, hot-clip ceiling. Runs: 55167 engine checks +
  443 QA checks + `misuc_tests_flp` + `misuc_headroom worstcase` all PASS,
  ASan-clean.
- **I — Browser / SOUNDS — DONE (Phase I, Sep 2026).** Persistent user-pack
  index (lofi/Samples + drum kits, kit auto-detected by folder name);
  Packs/Favourites/Recent category nodes; cross-pack filename search;
  Favourite/Recent persistence via QSettings; real drag-drop from the browser
  onto the playlist grid (full QDragManager protocol); Scan-Pack button
  drives `indexPackFolder()`, the same path as the QA.
  DoD met: `browser-ui` QA writes 10 real WAVs to a hermetic pack, indexes it,
  verifies pack listing (10 files), search matches, favourites + recent, then
  drops the 10 samples onto the grid — 10 clips land on track 0 at strictly
  increasing beats.
- **J — UX/HintBar/Inspector/icons/commands.** Per-control hints; inspector
  overlap fix; vector icon set completion; central command expansion;
  keyboard shortcuts doc.
  DoD: every control has a hint; no clipped inspector text at any window size.
- **K — Workspace/docking/flicker root cause.** Resize/dock robustness; the
  earlier flicker fix (WA_OpaquePaintEvent) verified at 4K + HDPI; layout
  presets persist.  **[DONE Sep 2026]**
- **L — Automation.** Pan/volume/param lanes beyond gain; per-lane record;
  curve/point editing; interop with undo + export.  **[DONE Sep 2026]** —
  gain/volume/pan lanes per-sample in process() (live == export), persisted
  per-lane record arm, param-aware Playlist band editing; 55184 tests + 443 QA
  green. FX control-port "param" lanes remain a follow-up.
- **M — Final QA / package / reinstall.** Full suite green headless; installed
  build re-verified; beginner tutorial `docs/MISUC_BEGINNER_TUTORIAL.txt`
  describes only real functionality; versioning documented.  **[DONE Sep 2026]**
  — 55184 tests + 443 QA + flp + headroom worstcase green; tutorial +
  versioning docs written; DESTDIR-staged install smoke-tested headless.
- **N — FX control-port automation.** Real-time control-rate automation of the
  fx insert chain (built-in EQ ports + LV2 control ports) via
  "fx:<chain>:<port>" lanes: engine sampling per segment, session lane
  creation/cleanup, Playlist band scaling/labels + per-insert port menu,
  Inspector per-parameter arm buttons, record sweep of insert values.
  **[DONE Sep 2026]** — 55220 tests + 443 QA + flp + headroom worstcase green;
  built-in-EQ low-pass automation provably audibly darkens the live render,
  and out-of-range targets are ignored without destabilizing the mix.
- **O — Per-block fx control smoothing.** Sub-block anti-zipper: inside each
  8192-frame render segment, fx inserts are now processed in 64-frame control
  blocks (750 Hz control rate at 48 kHz): the automation curve is sampled at
  the exact beat of each sub-block start and applied immediately before
  `process()`/`run()`. Replaces the old one-sample-per-segment hold; mid-segment
  curve steps land at the next 64-frame boundary instead of waiting until the
  next segment.
  **[DONE Sep 2026]** — 55228 tests + 443 QA + flp + headroom worstcase green;
  mid-segment LP step provably audible in first segment; ramp test proves
  smooth tracking across segment boundaries.
- **P — Export == live fx cadence.** Prove the offline renderer exercises the
  same 64-frame sub-block FX cadence as the live path (no live-only shortcut),
  and that a mid-segment curve step is heard in the exported file.
  **[DONE Sep 2026]** — bit-exact `memcmp` of monolithic live `process()` vs
  `renderToFile`'s 512-frame chunk loop; `midsegment_step_is_audible_in_export`
  mirrors the live test into the WAV; +14 checks (55242/0).
- **Q — Playlist band recording.** Dragging a Playlist automation band
  captures the performed values and merges them into the target lane on
  release so a drag+tape is one undo step.
  **[DONE Sep 2026]** — `liveBandDrag` plumbing (`Drag.liveVal`,
  `bandDragFinished` signal, `bandTape_` in MainWindow); `record-band-drag`
  QA case: fader parked −12 dB, dragged peak > 0.8 asserted at beat 0.5;
  +8 QA checks (451/0). Cleaned after itself so `export-ui` loud check
  stays hot.
- **R — Export metadata + per-project sample rate.** The project's metadata
  (title/artist) becomes the audio file's tags (RIFF INFO / Vorbis); the
  project's `transport.sampleRate` selects the export frame rate so a
  44.1 kHz bounce keeps exact duration/pitch at the target rate.
  **[DONE Sep 2026]** — `writeAudioFile`/`renderToFile` take title/artist,
  libsndfile `sf_set_string` writes RIFF INFO (WAV) / Vorbis (FLAC);
  `AudioBuffer` gains title/artist from the reader. A new
  `dsp::resampleLinear` converts the native-rate mix at render time when
  `exportSampleRate != engineRate`. ExportDialog gains Title/Artist fields
  and Sample rate combo; ProjectPage gains Artist field. Session artist
  persisted (`metadata.artist`). +28 engine checks (55270/0), +5 QA
  (456/0). ASan/UBSan clean on the resampler + tag paths.
- **S — Release packaging.** `make package` produces a DEB and a tarball;
  install + runpath hardening verified.
  **[DONE Sep 2026]** — CPack TGZ + DEB; staged-verify layout (bin + icons
  + .desktop); `misuc_app -platform offscreen` smoke-runs; relative
  `$ORIGIN` RUNPATH; packaging workflow documented in
  `docs/MISUC_VERSIONING.txt`.
- **Lat — Input-latency compensation.** Shift the take's capture latch by the
  engine's known input latency so the recorded span recenters onto what was
  actually heard (a 10 ms latency shifts ≈1.97–3.97 to ≈1.98–3.98 beats;
  sample payload untouched).
  **[DONE Sep 2026]** — `SetCaptureOffset` engine control (d0 = frames) +
  `AudioEngine::setInputLatencyMsOverride` (>=0 pins explicit ms, <0 uses
  `backend_->latency().inputMs`). `captureOffsetFrames()` accessor on both
  the engine and `RecordCapture` survives the take. Default 0 keeps headless
  tests exact. +19 engine checks (55299/0 total). Known limitations:
  per-track JSON offsets and monitor-playthrough RT delay line remain future
  work.
- **PR — Piano-roll toolset + metronome pass.** Complete the piano-roll
  tools pass (velocity nudge via Alt+Up/Down); ship a real metronome volume
  control, count-in length cycling, session persistence, and guarantee the
  click is a monitoring aid only — excluded from takes and offline exports.
  **[DONE Sep 2026]** — `PianoRoll::nudgeVelocity` (+ Shift = ±12 on the
  selection, clamp 1–127, one undo step); `Metronome::volume()` + engine
  `f2` control (clamped 0–1, default 0.9); transport-bar wheel (±0.05/0.1
  steps) + right-click cycles 2→4→1 beats; `.misuc` persistence
  (`transport.metronomeOn/Volume/countInBeats`, additive, schema stays v3);
  `applyMetronomeToEngine()` on every session-restore path; `offlineRender_`
  flag prevents `process()` from re-enabling ticking mid-bounce (latent
  click-leak fixed). +24 engine checks +21 QA checks (55299/0 + 477/0);
  ASan-clean.

## 5. First implementation sprint (Phase A task list)

1. `masterBus` stage: limiter/softclip + true-peak metering behind a decoupled
   DSP unit (live == export). 2. Headroom policy decision (below) wired into
   new-project defaults. 3. Clip-indicator: set where clipping samples occur.
4. `misuc_headroom` gain: add per-track true-peak + worst-case routing stress.
5. xrun soak test; installed-build verification. 6. Report per required format.

## 6. Decisions (user-approved, Sep 2026)

1. **Headroom policy:** master limiter holds the ceiling (~−1 dBFS) with faders
   kept at unity; true-peak meters + clip indicator show where headroom runs
   out. Default limiter on; no staged-fader defaults.
2. **FLP reader:** GO — clean-room structural reader in Phase B (user's own
   projects; informs `.misuc` v3 + optional structural import). No FL data
   copied. **DONE (Phase B, Sep 2026):** `misuc_flp` CLI + `FlpReader` lib;
   464-file parse 0 err / 0 truncated; full event-ID histograms identical to
   the pyflp oracle on 17 real projects; 16-field report in
   `MISUC_CURRENT_STATUS.txt`. Next slice: `.misuc` v3 schema.
3. **Browser SOUNDS — DONE (Phase I):** persistent user-pack index (`browser/packs` via QSettings); pack categories under a "Packs" tree node with kit auto-detection by folder name; initial seed probes the Windows-partition paths (`/media/<USER>/Windows/Downloads/{lofi,lofi/Samples}` + drum kits + `Projects/*KIT*`) at first launch (only existing dirs are kept); cross-pack filename search; favourites + recent persisted; real DnD from the browser onto the playlist grid via the full QDragManager protocol.
4. **QA scope guard:** keep `misuc_qa all` shared-MainWindow mode out of
   scope; run per-suite checks only. Follow-up: split shared-win cases into
   isolated engines.
5. **Phase P/R/S batch — DONE (Sep 2026):** user selected all four next
   phases (P export-cadence, Q band-recording, R export metadata/resampler,
   S release packaging) to be implemented together. P proved export == live
   is bit-exact and that mid-segment fx steps land in the file; R added
   title/artist tags (RIFF INFO + Vorbis), per-project sample-rate export
   (new `dsp::resampleLinear`), and wired them through ExportDialog/
   ProjectPage/session JSON; S turned `make package` into DEB+tarball and
   hardened the staged install layout.