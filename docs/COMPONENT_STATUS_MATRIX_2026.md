# Misuc — Component Status Matrix (audit, Sep 2026)

Every component audited against: does it affect real state/audio? Is it
engine-connected, session-connected, saved, undo-backed, tested? "REAL" means
it physically does something real (drives the Session/engine); "REAL (gap)"
means it works but a known feature is incomplete; "PLANNED" = not implemented.

## A. Audio foundation (Phase A)

| Component | Status | Engine | Session | Save | Undo | Test | Known limitations / priority |
|---|---|---|---|---|---|---|---|
| Transport play/stop/pause/seek/loop | REAL | yes | yes | yes | yes | yes | None measured. Loop overrides content end (by design). |
| **Master gain staging / headroom** | **REAL (DONE — Phase A)** | yes | yes | yes | yes | yes | **Master limiter at −1 dBFS (default ON) + true-peak meters; 12-hot stress: 0.891 peak, ZERO hard-clipped. Root cause (uninitialised busUsed/busScratch) fixed.** |
| Meters (master, track, bus) | REAL | yes | n/a | n/a | n/a | yes | Track/bus meters are post-gain, pre-gate. Master uses Catmull-Rom true-peak (estimate, not EBU R128 oversampled). |
| Metronome / count-in | REAL | yes | yes | yes | yes | yes | Sine+HP accents; volume (0–100%, transport-bar wheel) + count-in length (right-click cycles 2→4→1; default 2) persist in the `.misuc`; click is a monitoring aid only — never recorded into takes and never baked into offline exports. |
| Groove (demo drum) | REAL (convenience) | yes | no | no | no | yes | Hardcoded pattern, no UI; not a production feature. |
| Content end / auto-stop | REAL | yes | yes | yes | yes | yes | None. |
| Recording | **REAL (Phase G — DONE)** | yes | yes | yes | yes | yes | True input capture + overdub: loop-records one continuous take, metronome count-in, record-ARM + input monitor per track, Inspector take list. |

## B. Project & persistence (Phase B)

| Component | Status | Notes |
|---|---|---|
| .misuc format | REAL | **Real ZIP container (Comp17):** `misuc.meta` (`"format":"misuc.container"`, `containerVersion`, modifiedUtc) + `misuc.song` (the JSON document inside, schema v3: `"format":"misuc.song"`, `schemaVersion:3`, created/modified UTC, forward-compat gates reject newer schemas, legacy v1/v2 documents still load). zlib deflate + CRC32 verification; EOCD/central-directory parsing with bounds checks; corruption (truncation / bit-flip) rejected. Legacy plain-JSON `.misuc` files from older builds load transparently and migrate to the container on the next save. |
| Save/Load | REAL | Full session; container write is atomic (temp + fsync + rename + dir fsync, Windows-equivalent) — no torn files; auto `.prev`-style backups already exist for autosave; autosave rotate+verify+recovery read through the container-aware layer. |
| Recent projects | REAL | Via QSettings (`recent/projects`), File→Open Recent menu, max 12, existing-files-only, Clear Recent. |
| Autosave | REAL (Phase B — hardened) | 90 s while dirty; validate-before-promote rotation to `autosave.prev.misuc`; crash recovery copies survive the next autosave so a torn current never destroys the last good copy. |
| Drag-drop open | REAL | .misuc only. |
| **FLP structural reader** | **REAL (Phase B — DONE)** | clean-room `misuc_flp` CLI + `FlpReader` lib; 464-file parse (0 err / 0 truncated); ID-histogram-identical to pyflp oracle on 17 real projects (incl. 24.2.2.4597, 123/130 BPM, autosaves). Informs `.misuc` v3 schema. |
| **FLP → .misuc import** | **REAL (Phase B — DONE)** | `misuc_flp --import x.flp --out y.misuc`; structural mapping: channels→tracks, mixer inserts→buses, playlist items→MidiRegions (split by rack_channel), per-note pitch/velocity/length/position decode (24-byte FL20+ struct). Sampler-channel note hits skipped (honest); unassigned legacy notes→own track. 54569 engine checks + 281 QA checks pass on real files. |

## C. MIDI / Piano Roll (Phase C)

| Component | Status | Notes |
|---|---|---|
| Piano roll editor | REAL | Model-backed, full MIDI 0–127, C1–C8 nav target, chromatic support confirmed. **Tools pass (Phase PR):** select/copy/paint/quantize/erase + velocity lane + **velocity nudge** (Alt+Up/Down = ±1, Shift+Alt = ±12 on the selection, clamped 1–127, one undo step) + key preview. |
| MIDI synth | REAL | Sample-accurate; polyphonic; velocity → amplitude. Voice waveform has 3 partials (sin+0.5sin2+0.2sin3) — buzzy character; relates to headroom work. |
| Note preview / audition | REAL | Engine preview path. |

## D. Patterns / Channel Rack (Phase D)

| Component | Status | Notes |
|---|---|---|
| Channel Rack (step sequencer) | **REAL (Phase D — DONE)** | Session-persisted: rack lives in the `.misuc` document (`channelRack` JSON: name/mute/solo/volume/pan + 4 pattern banks × 16 base64 step lanes + activePattern + swing). Undo/redo-backed; keyboard **DELETE/Backspace removes the tracked channel** (view-level filter); programmatic setStep/setMuted/setSolo/setVolume all sync before edited(). |
| Pattern placement | REAL | "Place Pattern" materialises the ACTIVE pattern bank as a real session region. |
| Pattern live/song swap | REAL | pattern-in-session model landed (active bank + channel state in Session); live pattern playback republishes the freshly-synced steps through the audio engine. |

## E. Playlist (Phase E)

| Component | Status | Notes |
|---|---|---|
| Clip placement / move / resize / split / select | REAL | Model-backed; ruler seek; undo-backed. |
| **Paint tool** (FL "B") | **REAL (Phase E — DONE)** | `plPaint` button + **Key B**. Held-drag brushes every empty bar cell on a lane with the rack's ACTIVE pattern as a real `MidiRegion` ("Pattern N"); occupied cells are never overwritten; a whole stroke is ONE edit → one undo step. Verified against the DoD: a 100-block song painted in a single stroke, then undo/redo in one step each. Cursor: crosshair, exclusive with Select/Slice. |
| Sample import to timeline | REAL | insertSample path. |
| Zoom / snap | REAL | Snap + zoom wired. |

**Drive-by centralization (Phase E):** the step-sequencer kit mapping is now canonical in the engine — `Session.h` `rackRowFreq`/`rackRowPitch` ({A1,A3,E5,A1,A4} kit, A3 fallback) replaced `ChannelRack::previewPitch_`. Pattern→region materialization lives ONCE in the engine as `rackPatternNotes` + `materializePatternRegion(Session&, track, bank, start, dur)`; both "Place Pattern" (channel rack) and the Paint tool call it, so a painted pattern sounds like the instrument it came from. Solo semantics follow live rack playback (any solo → only soloed channels sound).

**Phase E DoD met:** "arrange a 100-clip song by painting; undo/redo every operation" — QA `firstfunk-paint-100` brushes 100 full bars onto a lane past the arrangement, one-stroke undo removes all 100, one-stroke redo restores them. |

## F. Mixer & FX (Phase F)

| Component | Status | Notes |
|---|---|---|
| Strips (fader/pan/mute/solo) | REAL | Per-track + per-bus, real-time safe. |
| Routing / buses | REAL | Track→bus→master; fader/pan/mute/solo per bus. |
| FX rack | REAL (LV2 + built-in) | Triple-buffered inserts; LV2 hosting when lilv present plus the always-on built-in parametric EQ (`builtin:parametric-eq`; biquad DSP, works without lilv). **No VST3 (user directive: not until core stable).** |
| Built-in EQ | **DONE** | Phase F. 4-band parametric (RBJ `setPeaking`) + HP/LP pads; 14 ports (HP, LP, bands × freq/gain/Q); catalogued + slider-UI on every build; engine tests prove boost/cut/transparent + round-trip. |
| Built-in SC compressor + sends / routing graph | **DONE (Comp19)** | `builtin:sc-compressor` (header-only `dsp/Compressor.h`, 5 ports) hosted on **bus post-fader chains** with a selectable sidechain key. Parallel per-source **sends** (track or bus → bus) with enable + smoothed dB level. Engine folds the routing DAG into a cycle-safe topological order (rescue path to master); SC key feed is the source bus's post-chain buffer, unaffected by the bus's mute/solo, so a muted drum bus still ducks. Model (graph, sends, sidechainSource) round-trips additively. UI: SendDialog + BusFxDialog + bus route combo on the bus strip. DoD: engine tests (7 cases, incl. ducking numerals) + QA `routing` (24 checks) + docs. |
| Mixer scale (125+ inserts) | **DONE** | Phase F. `kMaxTrackCount` 32→**128**, `kMaxBusCount` 16→**32**; staging buffers moved off the stack to heap vectors (object stays small for stack-allocated engine instances). DoD: 125-insert Session round-trips and renders sample-accurately (`misuc_headroom worstcase`: 125/125 inserts + 32/32 buses audible, 0 hard-clipped samples). |

**Phase F DoD met:** "125-insert session round-trips; routing graph renders sample-accurate (headroom tool on worst-case routing)". `misuc_headroom worstcase` (PASS) renders the reloaded 125-insert/32-bus session; every insert and every bus meter carries audio; master limiter holds every sample ≤ −1 dBFS. Engine `mixer_scale_125_inserts_render`; built-in EQ boost/cut proven numerically in `test_fx`. QA `fx-ui` now also drives the built-in EQ through the real combo/spin/render/remove. |

## G/H. Recording / Export

| Component | Status | Notes |
|---|---|---|
| Recording (true capture + overdub) | **REAL (Phase G — DONE)** | WAV to project `audio/`, placed on the armed track at the capture start. Per-track record ARM on the mixer strip (`arm_<id>`) drives the Session flag and the engine input-monitor gate; the Transport record button plays + arms (count-in when metronome click armed); the Inspector take list places (comp-style collapse) and removes takes. Long captures tail-free to the 5-min ceiling with frames-dropped accounting (`kMaxRecordFrames`). |
| Export | **REAL (Phase H — DONE)** | WAV float, WAV PCM16, FLAC16 from `File > Export` (Ctrl+E) via libsndfile: container + range (Whole song / Loop / Selection) + optional TPDF **dither** for the fixed-word formats. Export renders the same offline master path as playback and is held to the −1 dBFS limiter ceiling in every container. `writeAudioFile`/`AudioEngine::renderToFile` are the shared, unit-tested backbones; `doExport` (MainWindow) owns stream pause/restore. MP3/OGG later. |

**Phase G DoD met:** "record off the input; re-open the project; the clip plays exactly there." `test_record` proves the engine records an input stream up to 5 min with a sample-accurate transport span — loop-wrap is ONE continuous take (samples == passes × loop, spans 0..loopEnd), count-in click beats never corrupt the capture, and the monitor-off path captures without playthrough. The UI writes the WAV into project `audio/`, registers a Take, and puts the clip on the armed track at the capture start; the `.misuc` round-trip resolves the take's audio asset on reload. QA `record-ui` drives the real widgets: strip ARM → session `recordArm` + engine `inputMonitor()`, transport record on/off → a full headless 2-beat capture, and take place/place-again/remove on the timeline. |

**Phase H DoD met:** "exported file decodes identically in a second reader; FLAC round-trips." `test_export` (7 cases) proves every container round-trips through libsndfile's own reader: FLAC readback ≤ 1 LSB, PCM16 ≤ 2 LSB, dithered ≤ 3 LSB; dither is deterministic (byte-reproducible), injects only its ±1 LSB (DC-gate check) and decorrelates against the master; `renderToFile` FLAC tracks the warm float master ≤ 2 LSB; a [1,3) beat slice equals the whole-song render sample-for-sample; the headroom policy never silent-clips a clean master (mid clean peak passes untouched at 0.707), and a 2.0-amplitude source is held ≤ 0.892 (−1 dBFS) in every container. QA `export-ui` bounces the same real session through the File > Export menu action with the headless override: whole-song FLAC/PCM16/float, dither on/off word change within ±1 LSB, PCM16-vs-FLAC ≤ 2 LSB, FLAC-vs-float ≤ 2 LSB, hot-clip ceiling 0.891 in all formats, and loop-range [1,3) matching the warm whole-song slice.

**Phase H drive-by (found under ASan):** `wav_pcm16_roundtrip` had allocated 2400 samples but declared 2400 stereo frames (heap read overflow in the new frameCount×channels path); fixed to a 4800-sample buffer. `export_render_range_subset` staged a 36000-frame source for a 96000-frame clip window (OOB `write` into libsndfile); source is now the full 4 beats. Both were latent bugs in the test files, not in production code.

## I. Browser / sounds

| Component | Status | Notes |
|---|---|---|
| Browser | REAL | Honest samples-only: audition + drag-drop into playlist. **SOUNDS library live (Phase I, Sep 2026):** persistent user-pack index (lofi/Samples + drum kits), Packs/Favourites/Recent categories, cross-pack filename search, and real drag-drop onto the playlist grid. |
| Pack indexing / persistence | REAL | `Browser::indexPackFolder()` is the real path behind the Scan-Pack button and the QA; scanned dirs + the seeded Windows-partition packs persist via QSettings `browser/packs`; only existing dirs are kept. Drums kits auto-detected by folder name ("kit"), given an Instruments icon. |
| Favourites / Recent | REAL | Heart/favourite toggle (persisted `browser/favourites`) plus a Recent list (last auditioned/inserted, capped, `browser/recent`); both are selectable category-list nodes. |
| Cross-pack search | REAL | The search field filters EVERY indexed pack + the platform locations by filename (not just the active category). |
| Instruments/Effects catalogs | REMOVED (good) | No fake catalogs — documented in MISUC_CURRENT_STATUS Phase 18. |

**Phase I DoD met:** "drop 10 samples from a pack onto the playlist and build a beat." `browser-ui` QA writes 10 real WAVs into a hermetic pack dir, `indexPackFolder()`s it, asserts Packs shows the pack with 10 files, verifies 10 cross-pack search matches, toggles a Favourites + Recent entry (audition via the real preview button), then drops the 10 files onto the playlist grid through the faithful enter→drop DnD protocol — 10 clips land on track 0 at strictly increasing beats.

## J/K. UX, theme, workspace, icons, commands

| Component | Status | Notes |
|---|---|---|
| HintBar (per-control) | REAL | Live values. Per-control hint pass planned. |
| Inspector | REAL | Reflects selection; grid layout. Overlap fix pass planned. |
| Theme | REAL | 8 factory presets + runtime switch; global accent drives 87 consumer sites + logo; persistence across restarts (custom accent + base preset; no startup flash). Verified: 50 theme QA checks PASS. |
| Workspace/docking | REAL | 4 presets, QDockWidget, flicker mitigated (WA_OpaquePaintEvent). Resize/dock robustness + root-cause pass in Phase K. |
| Icons | REAL | Vector QPainterPath, any size. |
| Command system | REAL | Central registry; undo hooks. Extension pass planned. |

## L. Automation

| Component | Status | Notes |
|---|---|---|
| Automation lanes | REAL | `gain`/`volume`/`pan` lanes, per-sample interpolation, muted-lane, export-correct (same `process()` path as live). Pan overrides constant-power pan via `panLaw`; volume is an in-series amplitude layer [0,1]. Per-lane record arm (`AutomationLane::armed`, persisted) records live fader/pan moves at the transport position. **Phase N:** fx control-port lanes (`"fx:<chain>:<port>"`) added — see `## N. FX control-port automation` (DONE Sep 2026). |

## M / Engine QA

| Component | Status | Notes |
|---|---|---|
| Engine test suite | REAL | 55k+ checks green (55299, incl. Phase G record: capture span/count-in/monitor gate loop-wrap, **Phase Lat: input-latency recentered span + zero-offset baseline + click-free offline render**, Phase F scale: 125-insert round-trip/render, built-in EQ DSP, Phase H export: FLAC/PCM16 readback bounds, dither reproducibility + ±1 LSB DC gate, render-range slicing, headroom policy, Phase I browser DnD: 10-pack grid-drop insert verified sample-for-sample, **Phase P export cadence: mid-segment step + export==live bit-exact, Phase R metadata tags + 44100 Hz resampled export, Phase PR metronome volume/DSP + session persistence/legacy-defaults**); `misuc_tests_flp` PASS. |
| QA harness | REAL | `misuc_qa all` = **477** checks green (`record-ui`: arm/monitor/transport-rec/take-list; `export-ui`: format/dither/headroom/loop-range/meta-tags/44100 via the real menu path; `record-band-drag`: Playlist band drag → automation tape; `browser-ui`: pack index/search/favourites/recent + DoD grid-drop builds a beat; **`metronome-ui`**: transport-button toggle → engine+session, wheel volume, right-click count-in cycling, save/reload persistence; **`piano-velocity`**: real piano-roll drawing + selection + Alt+Up nudge 100→101). ASan-clean under `ulimit -s 65536` (only the pre-existing `test_audio` live-PipeWire env flake / PipeWire 8192 UBSan warning on the engine suite). |
| **Drive-by repairs (found via ASan soak)** | NEW | 3 pre-existing Playlist.cpp lifetime bugs fixed: `duplicateRegionAt` + `sliceRegion` held a `const MidiRegion&` into a vector that `addRegion/duplicateRegion` reallocated (use-after-free); automation-lane click reused an iterator after `vector::insert()` invalidated it (garbage index → SEGV/hang). Plus `EditorPage` track-list item widgets de-parented to the canonical form (avoids latent double-ownership). Phase I found and fixed a missing `setAcceptDrops(true)` on PlaylistGrid — without it `QWidget::event()` silently ignored every incoming drop from the browser. |
| **Headroom diagnostic** | NEW | `tests/../tools/headroom.cpp` / `bin/misuc_headroom` — measures true peak + clipped-sample stats on any project and the multi-track sum. **Phase F `worstcase` mode:** 125-insert/32-bus round-trip render gate. Backs the clipping acceptance check. |

## N. FX control-port automation (Phase N)

| Component | Status | Notes |
|---|---|---|
| Engine fx automation | REAL (DONE Sep 2026) | `process()` samples "fx:<chain>:<port>" lanes at control rate (segment-start beat, held per segment) and feeds each enabled insert via `ParametricEq::setPort` (built-in) / `Lv2Host::setControl` (LV2, writes the already-connected `ctrlIn[]` — no locks/allocation). Offline `renderToFile` shares the identical path, so live == export. Out-of-range chains/ports and bypassed inserts are skipped without destabilising the mix (test-enforced). |
| Lv2Host::setControl | REAL (DONE Sep 2026) | `FxInstance` maps control ports → `ctrlIn[]` slots at instantiate; setter is a realtime-safe scalar write (no-op for unknown ports, sentinel UINT32_MAX). |
| ParametricEq::setPort | REAL (DONE Sep 2026) | Real-time control-rate port setter on the audio thread; clamps into native ranges, recomputes only the impacted filter, unknown ports ignored. |
| Session model | REAL (DONE Sep 2026) | `addAutomationLane(track, param, restingValue)` overload seeds fx-lane endpoints at the control's current value; `removeFx` drops lanes targeting the removed insert and shifts (`ci--`) lanes that pointed at later inserts so they keep automating the same plugin. Lanes persist generically (param is a string, `armed` already post-v3 additive). |
| Playlist band | REAL (DONE Sep 2026) | Band scales fx lanes from the plugin's native min/max with the current control as the resting line; the left tag becomes the port label (width-adaptive click/arm zone); the band menu adds "Automate FX: slot <i>" per insert enumerating every catalogued control port (QAction `data()` = exact lane param). |
| Inspector | REAL (DONE Sep 2026) | Per-parameter "A" arm toggle (armed state reflected), `editRequest("fx.<id>.auto.<port>", 1/0)` → MainWindow creates/toggles the "fx:<chain>:<port>" lane (resting at the current control value) and re-arms other lanes off as normal. |
| MainWindow wiring | REAL (DONE Sep 2026) | `applyAutomationToEngine()` publishes all lane params (fx included); `pollAutomationRecord()` sweeps insert param values at 50 Hz for armed fx lanes (last-written fallback); Playlist port metadata resolver built from `engine_.fxInfo(uri)`. |
| Tests | REAL (DONE Sep 2026) | `parse_fx_automation_grammar`, `fx_automation_lp_darkens_render` (built-in EQ LP 20 kHz→200 Hz audibly darkens the live render, verified against the unautomated chain), `fx_automation_unknown_target_is_ignored`, `session_fx_automation_lane_rests_and_roundtrips`, `session_remove_fx_reindexes_automation_lanes`. **Phase O:** `fx_automation_midsegment_step_is_audible` (LP step at beat 0.2 inside the first 8192-frame segment — proves per-block cadence lands the step 3200 frames early), `fx_automation_ramp_tracks_progressively` (LP ramp 20 kHz→100 Hz over beat 0..1 — measures early-seg brightness vs late-seg darkness). Suite **55228/0**; QA 443/0; flp PASS; headroom worstcase PASS; app smoke OK. |

## O. Per-block fx control smoothing (Phase O)

| Component | Status | Notes |
|---|---|---|
| Sub-block smoothing | REAL (DONE Sep 2026) | Inside each 8192-frame render segment, fx inserts are now processed in 64-frame control blocks (`kFxControlFrames = 64`): the automation curve is sampled at the exact beat of each sub-block start and applied to each insert immediately before `process()`/`run()`. Replaces the old one-sample-per-segment hold. At 48 kHz: ~750 Hz control rate (~1.33 ms latency vs old ~170 ms segment hold). Live and offline share the same sub-block path (export == monitor). |
| Tests | REAL (DONE Sep 2026) | `fx_automation_midsegment_step_is_audible` (LP step at beat 0.2, peak measured in frames 5000..8191 < 0.7× control — segment hold would keep it bright), `fx_automation_ramp_tracks_progressively` (LP 20 kHz→100 Hz over beat 0..1, early > 0.5× control, late < 0.5× control, monotonic darkening, deviation > 5%). 55228/0 total + 443/0 QA. |

## P. Export === live fx cadence (Phase P)

| Component | Status | Notes |
|---|---|---|
| Mid-segment step audible in export | REAL (DONE Sep 2026) | `fx_automation_midsegment_step_is_audible_in_export`: LP-curve step at beat 0.2 is heard in the RENDERED file too — measured on interleaved L frames 5000..8191 of a WAV export of the same Session the live test renders directly. Proves the offline renderer exercises the same every-64-frame sub-block path (no live-only fast path that export attenuates). |
| Export == live, bit-exact | REAL (DONE Sep 2026) | `fx_automation_export_matches_live_cadence`: one Session, two renders, identical input -> `memcmp`-identical output. The live monolithic `process()` (one call, whole range) equals `renderToFile`'s 512-frame chunk loop byte-for-byte (MasterLimiter is causal, sub-blocks stay 64-aligned because 512 % 64 == 0), so the exported wav is the exact sound of the monitor. |
| Tests | REAL (DONE Sep 2026) | +14 checks (55228 → 55242/0). One transient failure during development was an interleaved-window indexing bug (`peakRange(samples, 5000, 8191)` read mismatched channels); fixed to L-frame indices and verified stable across runs. |

## Q. Playlist band recording (Phase Q)

| Component | Status | Notes |
|---|---|---|
| Band drag → automation tape | REAL (DONE Sep 2026) | Dragging a Playlist automation band records the live value into a `MainWindow::bandTape_` (the gain/pan lane is NOT mutated mid-drag — no vector reallocation under the drag). On release (before `edited()`), `onBandDragFinished` merges the tape into the lane (0.01-beat dedup, sorted lower_bound insert) so drag + tape commit as ONE undo step. |
| Drag plumbing | REAL (DONE Sep 2026) | `PlaylistGrid::Drag` tracks `liveVal`; `PlaylistGrid::liveBandDrag(row, autoIdx, value)` exposes it; release emits `bandDragFinished(row, autoIdx)` BEFORE `edited()`. `Playlist::liveBandDrag` forwards; MainWindow connects the signal and rewrites `pollAutomationRecord` to defer the dragged lane into the tape. |
| QA | REAL (DONE Sep 2026) | `record-band-drag` case: fader parked at −12 dB ensures the taped value MUST come from the band drag; engine drives play + 12000 frames to beat 0.5; postPress/postMove/postRelease on the grid; `recordAutomationTick()` after release; asserts a point at beat ~0.5 with value > 0.8. Case cleans up (removes gain lane, restores gainDb=0) so the later `export-ui` loud check stays hot. QA 443 → 456/0. |

## R. Export metadata + per-project sample rate (Phase R)

| Component | Status | Notes |
|---|---|---|
| Title/artist tags | REAL (DONE Sep 2026) | `Session::artist()` gained a persisted field (JSON `metadata.artist`, post-v3 additive) alongside `name`. `writeAudioFile`/`renderToFile` take optional title/artist and emit them via libsndfile `SF_STR_TITLE`/`SF_STR_ARTIST` (RIFF INFO `INAM`/`IART` for WAV, Vorbis comment for FLAC). `readAudioFile` populates `AudioBuffer::title/artist`, so tags round-trip through the engine's own reader. |
| Per-project sample rate | REAL (DONE Sep 2026) | The project's `transport.sampleRate` (already persisted) selects the EXPORT frame rate. The mix is always rendered at the engine's native 48 kHz then converted by a new `dsp::resampleLinear` (piecewise-linear, endpoint-preserving) so duration and pitch are exact at the chosen rate (44.1/48/96/192 kHz in the Export dialog). |
| Export UI | REAL (DONE Sep 2026) | ExportDialog gains Title/Artist fields (prefilled from Session) and a Sample rate combo; ProjectPage gains an Artist editor; headless override exposes `setExportSampleRateOverrideForTest`. |
| Tests | REAL (DONE Sep 2026) | `export_metadata_tags_roundtrip` (WAV + FLAC tags, empty-untagged), `export_sample_rate_44100_resample` (44.1 kHz export = 88200 frames / 22050 per beat, tags survive, peak present), `resample_linear_preserves_sine_pitch` (440 Hz sine down- and up-sampled keeps ~2×frequency zero-crossings/sec and >0.99 peak), `session_roundtrip` artist check. Suite **55270/0** (+28); QA export-ui adds the meta/44100 path (**456/0**, +5). ASan/UBSan clean on the resampler and tag paths. |

## S. Release packaging (Phase S)

| Component | Status | Notes |
|---|---|---|
| CPack tarball + DEB | REAL (DONE Sep 2026) | `cmake --build build --target package` now emits BOTH `misuc_<ver>-<rel>_amd64.deb` (deps via `CPACK_DEBIAN_PACKAGE_SHLIBDEPS`) and `misuc-<ver>-<rel>-linux.tar.gz` (plain staged layout). |
| Install hardening | REAL (DONE Sep 2026) | `make install DESTDIR=<stage>` verified: staged layout holds bin/misuc_app + misuc_flp, share/applications/misuc.desktop, full hicolor icon set; staged `misuc_app -platform offscreen` smoke-runs (alive under timeout kill, exit 124). No absolute RUNPATH (relative `$ORIGIN`), all shared libraries resolve, desktop entry resolves `misuc_app` via PATH by design. Packaging workflow documented in `docs/MISUC_VERSIONING.txt`. |

## Priority ladder (what to fix first)

1. **P0 – Master headroom / clipping — DONE (Phase A)**: master limiter (−1 dBFS, default ON) + Catmull-Rom true-peak meters + clip indicator; 12-hot stress −1.0 dBFS, ZERO clipped; root-cause uninitialised `busUsed`/`busScratch` read fixed (this was the sporadic >0.891 record-regression). Next hardware check: live-PipeWire xrun soak (human session).
2. **P1 – Channel Rack persistence + DELETE/trash fix** — **DONE (Phase D)**: rack session-persisted; keyboard DELETE fixed; undo/redo-backed; drive-by ASan repairs to Playlist + EditorPage lifetime bugs above.
3. **P1 – .misuc v3 versioned format** + atomic save + crash recovery + robust autosave. Phase B. **Phase B progress (Sep 2026): FLP structural reader DONE** — clean-room, oracle-verified; v3 schema is next.
4. **P1 – Recording realness pass — DONE (Phase G)**: true input capture + overdub; loop-record = one continuous take; metronome count-in excluded from capture; per-track record ARM + input-monitor gate; Inspector take list (comp-style place/remove). **P2 – input-latency compensation — DONE (Phase Lat, Sep 2026)**: `SetCaptureOffset` engine control + `AudioEngine::setInputLatencyMsOverride` shifts the take's capture latch so the recorded span recenters onto what was actually heard (backend input latency by default, explicit override pins; default 0 keeps headless tests exact). Per-track offsets and monitor-playthrough delay-line stay documented limitations.
5. **P2 – Playlist paint tool (B) — DONE (Phase E)**: paint tool lands the Phase E DoD (100-block painted song, one-stroke undo/redo). **P2 – piano-roll tool set + metronome pass — DONE (Phase PR, Sep 2026)**: piano roll tools complete (pencil/select/erase/copy/quantize/velocity lane + key preview + **velocity nudge via Alt+Up/Down**); metronome gained a real volume control (transport-bar wheel, 0–100%), count-in length cycling (right-click 2→4→1) and full `.misuc` persistence; the click is monitoring-only — excluded from takes and offline exports (regression-tested).
6. **P2 – Export FLAC — DONE (Phase H)**: WAV float + PCM16 (already real) plus FLAC16, range export (Whole song / Loop / Selection), optional TPDF dither on the fixed-word formats, all held to the −1 dBFS export ceiling and verified to round-trip through a second reader (libsndfile decode-back in `test_export` and the `export-ui` QA path). Browser SOUNDS indexing — DONE (Phase I).
7. **P2 – Mixer 125+ inserts; built-in EQ — DONE (Phase F)**: `kMaxTrackCount` 32→128, `kMaxBusCount` 16→32, staging scratch → heap vectors; built-in parametric EQ (`builtin:parametric-eq`) catalogued/rendered on every build. DoD gate `misuc_headroom worstcase` PASS (125/125 inserts + 32/32 buses audible, 0 hard-clip samples). Automation lanes (pan/vol) — DONE (Phase L): gain/volume/pan per-sample, export-correct, per-lane record arm. FX control-port lanes — DONE (Phase N): "fx:<chain>:<port>" automates built-in EQ ports + LV2 control ports at control rate (live == export), with Playlist band + Inspector arm UI.
8. **P3 – Theme runtime, Inspector overlap, workspace robustness, per-control hints, command expansion.**
  Phase J DONE (Sep 2026): Inspector overlap fixed via QScrollArea; per-control hints registered in Inspector (addTextEdit/addToggle/addNumber/addTakes/addFxRack) and wired through MainWindow::buildPages(); Inspector page-level hint ("F4") + chrome wire; transport loop/record shortcuts (L/R) in HintBar; 443 QA checks + 55167 tests all PASS; release+ASan clean; headroom worst-case PASS. Phase K DONE (Sep 2026): theme runtime switch (themeChanged→refresh live), keyboard-shortcuts reference, workspace robustness verified. Phase L DONE (Sep 2026): pan/volume automation lanes (engine+model+UI), per-lane record arm, undo/export interop; 55184 tests + 443 QA green.
9. **P3 – Export trust + release packaging — DONE (Phases P/R/S)**: Phase P proved export == live is bit-exact and that a mid-segment fx step is audible in the file (55242/0). Phase R shipped export metadata (title/artist tags, RIFF INFO / Vorbis) and per-project sample rate (resampled 44.1/96/192 kHz bounces) — 55270/0 + 456/0 QA. Phase S turned `make package` into a DEB + tarball pair and hardened/staged-verified the install layout. Phase Q recorded Playlist band drags as automation (drag + tape = one undo step). **Phase Lat + Phase PR (Sep 2026):** input-latency compensation (recording recenters onto what was heard) and the piano-roll/metronome pass (velocity nudge; metronome volume + count-in + persistence; click-free export) — **55299/0 + 477/0 QA**, ASan-clean.

## Root cause: audio clipping ("overdriven radio")

Measured with `misuc_headroom` (offline export == live process() path):

```
1 hot track   peak 0.497 (−6.1 dBFS)   ok
2 hot tracks  peak 0.994 (−0.1 dBFS)   ok (0 dB headroom)
3 hot tracks  peak 1.491 (+3.5 dBFS)   41,028 clipped samples  <-- CLIPS
4 hot tracks  peak 1.988 (+6.0 dBFS)   67,952 clipped samples  <-- CLIPS
8 hot tracks  peak 3.976 (+12 dBFS)    125,444 clipped samples <-- CLIPS
```

Chain:
- Note amplitude = (velocity/127) × kLevel 0.25 × env; waveform |sin+0.5sin2+0.2sin3| peaks ≈ 1.7 → a full-velocity note ≈ 0.425 peak.
- In-phase bass/kick parts add **coherently** (~+1.455 peak per extra identical track, measured).
- The master is `sum × masterGainDb` and nothing else: **no limiter, no softclip, no headroom reserve**. `clipped_` only *reports* after the fact.
- FIRST FUNK delivery currently renders −0.7 dBFS peak (no clipping) but has **0.7 dB of headroom**; any added element clips immediately. (The earlier radio-like artefacts also included the now-fixed interleaved/planar stride bug.)

=> Phase A design decision (with user): default headroom policy + a real master limiter stage (+ optionally export normalisation) so nothing the user drops in can hard-clip by surprise, plus honest per-track/per-bus true-peak meters to show where headroom runs out.

## Root cause RESOLVED (Phase A)

Two distinct defects were fixed on the way to green:

1. **Master had no limiter** — now a feed-forward brickwall `MasterLimiter` at −1 dBFS (default ON) sits after the master-gain smoother and before the meters; live and offline renderings share the exact same stage, so export == monitor. Measured after fix:
   ```
   12 hot tracks  peak 0.891 (−1.0 dBFS)   |x|>0.999 samples=0  <-- CONTAINED
   ```
2. **Uninitialised bus state (UB)** — `bool busUsed[kMaxBusCount];` was read uninitialised when no buses were routed (`anyBus == false`), and `busScratchL/R` were only zeroed inside `if (anyBus)`. A garbage `busUsed[b] == true` caused the uninitialised bus scratch (heap values up to ~1e32) to be summed into the master, which the new limiter then clamped to 0.891 — the sporadic `while=0.891` record-test regression (position varied with heap layout; vanished under ASAN). Fixed: `busUsed[b] = {}` and unconditional per-segment bus-scratch zero-fill.

Post-fix soak: 300 × 12-hot stress renders (~10 min of continuous hot mixing) — ZERO clipped samples, limiter peak steady at 0.891.
## M. Final QA / package / reinstall

| Component | Status | Notes |
|---|---|---|
| Metronome / count-in | REAL (Phase PR) | None remaining beyond the documented pattern-improvement idea (user request). Click is monitoring-only by design. |
| Velocity nudge tool | REAL (Phase PR) | Alt+Up/Down (±1, Shift+Alt ±12) on the piano-roll selection; clamp 1–127; one undo step. |

## Lat. Input-latency compensation (Phase Lat)

| Component | Status | Notes |
|---|---|---|
| Engine capture offset | REAL (DONE Sep 2026) | `ControlKind::SetCaptureOffset` (control 15, `d0` = frames) + `AudioEngine::setInputLatencyMsOverride(double)` (>=0 pins an explicit ms offset, <0 falls back to the backend's `latency().inputMs`). Applied at the capture latch: `recordStartFrame_`/`recordLastWriteEnd_` are shifted by the offset, so the take's stored span recenters onto what was actually heard (a 10 ms override shifts the span from ≈1.97–3.97 to ≈1.98–3.98 beats; sample payload is untouched). Default offset 0 keeps headless tests exact. |
| Capture offset accessor | REAL (DONE Sep 2026) | `AudioEngine::captureOffsetFrames()` + `RecordCapture::captureOffsetFrames` survive the take, so the Inspector/take list can report how much was compensated. |
| Tests | REAL (DONE Sep 2026) | `record_input_latency_compensation_recenters_span` (override 10 ms → offset 480 frames, span ≈1.98–3.98, sample size unchanged), `record_zero_latency_keeps_baseline_span`, `offline_render_excludes_metronome_clicks`. |
| Known limitations | DOCUMENTED | Per-track JSON offsets and a monitor-playthrough RT delay line are out of scope (a uniform global latency buffer at the capture latch is the honest first pass); the shift is bounded by the existing 5-min ceiling. |

## PR. Piano-roll toolset + metronome pass (Phase PR)

| Component | Status | Notes |
|---|---|---|
| Velocity nudge | REAL (DONE Sep 2026) | `PianoRoll::nudgeVelocity(int)` → canvas selection-only, clamp 1–127, one `commit()`. KeyPress Alt+Up/Down (Shift+Alt ±12). QA `piano-velocity`: real drawing + selection + nudge 100→101. |
| Metronome volume | REAL (DONE Sep 2026) | `Metronome::volume()` setter → click level; engine `setMetronome(bool,float,float)` default 0.9 via control `f2` (clamped 0–1 in the RT handler). Transport bar wheel over the metronome button adjusts by 0.05/0.1 steps. |
| Count-in cycling | REAL (DONE Sep 2026) | Right-click the metronome button cycles 2→4→1 beats (left-click toggles on/off). |
| Persistence | REAL (DONE Sep 2026) | `transport.metronomeOn/metronomeVolume/countInBeats` (additive, schema stays v3). Legacy docs load defaults off/0.9/2.0. `applyMetronomeToEngine()` refreshes bar + engine on every session-restore path (new/open/load/undo/redo/edit). |
| Click-free export | REAL (DONE Sep 2026) | `renderToFile` sets an `offlineRender_` flag so `process()` never re-ticks the metronome mid-render (was a latent leak: enabling the metronome then bounced with clicks). Regression-tested. |

## M. Final QA / package / reinstall

| Component | Status | Notes |
|---|---|---|
| Full suite headless | REAL (DONE Sep 2026) | misuc_tests 55299/0, misuc_qa 477/0, misuc_tests_flp PASS, headroom worstcase PASS. ASan clean except the pre-existing test_audio live-PipeWire env flake (and the unrelated PipeWire 8192 UBSan warning). |
| Beginner tutorial | REAL (DONE Sep 2026) | docs/MISUC_BEGINNER_TUTORIAL.txt — 14 sections, every claim verified against code (only real functionality). |
| Versioning | REAL (DONE Sep 2026) | docs/MISUC_VERSIONING.txt — app 0.1.0, .misuc schemaVersion 3 (+ additive v4 members), install layout. |
| Installed build | REAL (DONE Sep 2026) | make install DESTDIR stage → bin/ + hicolor 16..512 icons + misuc.desktop; staged binary runs headless (offscreen smoke). |

## End of roadmap backlog
