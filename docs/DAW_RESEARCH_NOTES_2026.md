# Misuc — DAW Research Notes (audit, Sep 2026)

What the provided reference materials can teach us, and what they cannot.
Research only — nothing here has been copied into Misuc.

## 1. Legal boundary

| Material | License/ownership | What we may do |
|---|---|---|
| LMMS 1.2.2 AppImage | **GPL open source** | Inspect freely; run it; consult its source/specs; it is a direct architectural reference for an open-source DAW (Beat+Baseline, Song Editor, FX, mixer). |
| Bitwig Studio 6.0.11 .deb | Proprietary, user-licensed | Install and run for **UX/behavior reference only**. Do not extract, copy, or embed any assets/presets/plugins. |
| FL Studio 26 installer .exe + `/media/oibhupi/Windows/Image-Line` | Proprietary, user-licensed | Cannot run on Linux. Use as **workflow/feature reference** from knowledge only. Never copy FL plugins, presets, or content. |
| `.flp` projects in `/media/oibhupi/Windows/Downloads/Projects` + `lofi/` | **User's own documents** | Clean-room structural reader for **interoperability** (extract tempo, channel rack, playlist, mixer, automation structure). Never reproduce FL-plugin preset bytes. |
| Sample packs & lofi samples | User-owned audio | Indexable by the Browser's SOUNDS library; usable in the user's own songs. Not redistributable. |

## 2. Reference inventory

| Path | What it is | Teaches us |
|---|---|---|
| `Downloads/lmms-1.2.2-linux-x86_64.AppImage` | GPL DAW (extracted to /tmp; `usr/share/lmms/{projects,presets,samples,themes}` + LV2/LADSPA plugin set) | Open-source template/preset/sample structure; how a FOSS DAW organises patterns (`projects/templates`), FX, and themes. Direct UX + architecture reference. |
| `Downloads/bitwig-studio-6.0.11.deb` | Bitwig 6 installer (installed package layout, EULA, Library) | Modern non-linear DAW: device racks, remote controls, Modulation, clip launcher. Behavioural target for Phase F (Mixer), L (Automation/Modulation), K (workspace). |
| `Downloads/flstudio_win64_26.1.6.5639.exe` | FL 26 installer (PE, Nullsoft SFX) | Workflow reference only (non-runnable here). Channel Rack / Playlist / Pattern / Browser mental model behind Phases C–E, I. |
| `Windows/Image-Line/…` | Full FL install + plugins + Loops | Reference for FL's effect/sequencer paradigm. Do not extract. |
| `Windows/Downloads/Projects/*.flp` | ~30 real productions (funk/lofi remakes) + autosaves | **The user's real composition anatomy**: which channels, which mixers, which sounds, how patterns/playlist are used. Directly drives the `.misuc` v3 format and the "firstfunk-class delivery" target. |
| `Windows/Downloads/lofi/lofi.flp` + `lofi/Samples` | Lo-fi project + its sample folder (Rim/Snare/808/909 kits, BWB claps…) | The exact sound palette for the lofi branch of delivery; feedstock for Browser SOUNDS. |
| `Windows/Downloads/lofi/Samples`, `Projects/*KIT*`, `LoFi Drum Kit 2022`, `Phonk Drum Kit` | User-owned royalty-free kits | Indexable sample library (see Phase I). |

## 3. FLP anatomy findings (verified on `lofi.flp`, `New one lets bang it.flp`)

- Magic `46 4c 68 64` = `FLhd` header record; then `FLdt` data chunks; embedded ASCII parameter/plugin names (e.g. `24.2.2.4597` FL version, `Fruity SQ Basic`, `CamelCrusher.dll`, presets `Tube Warmth`, `Destroyer`, `YouWereWarned`).
- 1.7 MB project embeds sample references, plugin params, pattern/playlist structure, mixer state, automation — all recoverable via a clean-room FLP reader.
- Autosave/backup naming pattern visible (`(autosaved at …)`, `(overwritten at …)`) — mirrors Misuc autosave + crash-recovery goals.

**So:** an FLP structural reader is a Phase-B–E companion deliverable so we can (a) understand the exact shape of the user's real projects, (b) define `.misuc` v3 feature-for-feature, and (c) offer an FLP import path (structure only) if the user wants it. None of FL's proprietary data is copied.

## 4. Mature-DAW behavioural baseline (targets for Misuc phases)

| Subsystem | FL Studio | Bitwig | LMMS | Ardour/REAPER | Misuc target |
|---|---|---|---|---|---|
| Pattern/channel | Channel Rack + Pattern (note + step modes) | Clip launcher (non-linear) | BB editor + Song editor | Items/tracks | Phase D: Channel Rack real, patterns in-session, note+step lanes |
| Piano roll | 0–127, C1–C8, tools | Piano roll + keys | Piano roll | MIDI editor | Phase C: full 0–127, toolbar tools |
| Playlist/clip | Paint tool (B), per-channel clip tracks | Clips in arranged timeline | Song editor | Regions | Phase E: paint/move/resize/split/select |
| Mixer | Insert tracks, routing, FX | Device chains, sends | FX chains, sends | Full mixing console | Phase F: 125+ inserts, routing, FX rack, built-in EQ |
| Recording | Audio/MIDI/overdub | Audio/MIDI | Audio/MIDI | Full | Phase G: true audio recording to project file |
| Export | MP3/WAV/FLAC… | WAV/FLAC… | WAV/FLAC/OGG/MP3 | WAV/FLAC/… | Phase H: WAV+FLAC first, more later |
| Loudness/clipping | Master limiter default | Master limiter | limiter on FX | dither/limiter optional | **Phase A: master limiter + headroom policy + meters** |
| Undo | system+per-tool | snapshot tree | limited | full snapshot | Phase snapshot undo + per-op (L) |

## 5. Open questions for the user (see end of ARCHITECTURE_PRIORITY_PLAN)

- FLP reader go/no-go; which reference to run under Wine/etc. — none needed for the plan now.
- Whether Browser "SOUNDS" should index the Windows-partition sample packs by default.
- Headroom policy: preserve current-maximal mix style (auto master trim at export) vs professional default (faders at −6-ish / master limiter).