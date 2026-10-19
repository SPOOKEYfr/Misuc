MISUC — IMPLEMENTATION ROADMAP (post-audit)
============================================

Source of truth for what remains to be done and the order to do it.
Each entry maps to a section in MISUC_MASTER_PROTOCOL.txt and is
executed with the full loop:
Research -> Inspect -> Design -> Implement -> Test -> Build -> Run ->
Verify -> Package -> Install -> Test installed -> Log -> Status.

Versioning canonical source: top-level CMakeLists.txt project() VERSION.
Current: 0.2.0. Bump patch for bug fixes, minor for features, major for
breaking changes. Every bump re-verifies the installed package reports it.

DEPENDENCIES (foundations before UI where required)
----------------------------------------------------
  Project Model        (COMPLETE)
     -> Audio Engine   (COMPLETE)
     -> Theme Engine   (COMPLETE, accent global per #15)
        -> Workspace   (#3)
           -> Editors  (Piano Roll #1, Channel Rack #2, Playlist #8/#9,
                         Mixer #16, Browser #4)
  Native .misuc format (#14)
     -> Autosave (#7 Part1) -> Recent (#7 Part2) -> Recovery
  FLP Import (#7 Parts 3-16)
     -> dirty-state/autosave safety fix (BLOCKER, do first)
  Record (#10) / Export (#13) / Metronome (#12) / Transport (#11) / Icons (#6) /
  Text overlap (#5) one at a time after the editors they touch stabilize.

COMPONENT ROADMAP (execution order)
------------------------------------
  0.1 Version system            COMPLETE   (0.2.0 canonical chain)
  0.2 Dev log + audit           COMPLETE   (MISUC_DEVELOPMENT_LOG.txt)

  01 FLP import data safety      COMPLETE     (dirty state after import →
      autosave covers imported sessions; data-loss risk closed; Section #7)
  02 Playlist-Piano 1/16 grid    COMPLETE     (quantize/grid resolution now
      selectable 1/32..1, default 1/16; loop/paste/duplicate honor it)
  03 Piano Roll vertical range   COMPLETE     (vertical zoom rowH 12..48 via
      toolbar slider + Ctrl+Shift+wheel; scrollable 0-127 pitch model;
      centralized dsp/Pitch.h midiToNoteName/noteNameToMidi + 420 checks)
  04 Channel Rack model/sched    COMPLETE     (#2: per-channel volume/pan
      editing, audible rack pan, reorder, per-step velocity, channel type +
      stable identity, variable pattern length 4..128)
  05 Workspace / docking         COMPLETE     (#3: resize/dock/tab/flicker;
      layout persistence; View-menu toggles w/ F5-F9 + Ctrl+Shift+I, checkable
      + state-synced; 70 QA ws checks; timer-hardened saveState/restoreState)
  06 Browser sounds library      COMPLETE     (#4: guaranteed app-data Sounds
      folder seeded with the project's real audio (demo_pad.wav →
      Instruments, never synthesized) + home Sounds/Samples; nested
      filesystem tree; single-click audition via engine preview pipeline with
      play/pause/stop + persisted volume + UI-clock progress; 96-bin async
      waveform thumbnails + metadata; drag-to-sampler-channel / drag-to-real
      playlist clip at the drop beat; rescan picks up new/removed sounds;
      soundFile persisted per sampler channel + project-relative assets;
      51 new sound-library QA checks, 596 baseline still PASS)
  07 Text overlap / layout       COMPLETE     (#5: reusable non-overlap fix —
      new header-only ElideLabel (right-elides, full text via tooltip only
      while elided, sizeHint keeps panels compact) applied to the Inspector
      title + every property row and the Browser preview title/meta; browser
      file rows carry the full path tooltip; Playlist custom paint elides
      clip/region labels instead of painting into neighbours; the Inspector
      property grid now rebuilds inside an owned host so stale rows are
      deleted (they previously kept painting over new rows); Channel Rack
      chrome no longer hard-shrinks below its controls (Qt compresses box
      spacing into overlaps) — its natural minimum is driven by the content;
      64 new text-overlap QA checks: geometry audit (no child escapes its
      visible parent, no visible siblings intersect) across every dock at
      min/narrow/medium/wide widths + central tabs, float/redock, ElideLabel
      elision/tooltip invariants under resizes; 647 baseline still PASS → 711)
  08 Icon + Hint Bar pass        COMPLETE     (#6: full Hint Bar coverage for
      every interactive control — Playlist Mute/Solo lane-cell hints (cell
      resolver now intercepts the M/S icon column before the generic lane),
      Editor toolbar (Add Track / Remove Selected/zoom/row-height/Snap/Grid/
      quantize/import/track list), Browser preview strip (scan/refresh/preview
      volume), Channel Rack Steps combo + Place Pattern (*registered with an
      icon while its native tooltip survives for the tooltip-based QA
      lookups*), Mixer add-bus + per-bus delete "×" + per-strip route combo,
      Audio Settings (device/rate/buffer/master+monitor gain/monitor/test
      tone/groove/apply), plus wiring Project/Theme/Editor/AudioSettings
      attachHints into MainWindow; real vector icons on every button that was
      previously text/unicode (Refresh glyph added as IconId::Refresh for the
      browser rescan); 30 new hint-ui QA checks drive Enter/Leave/MouseMove
      events over the real widgets and assert the HintBar settles to the right
      name, no stale names on leave, icons present, tooltips preserved →
      741 QA checks total, 55753 unit checks, FLP suite, packaged .deb +
      installed launch all PASS)
  09 Autosave config + recent    COMPLETE     (#7 Parts 1-2: autosave interval
      made configurable via QSettings `autosave/intervalSeconds` (Off/1/5/10/
      15/30/60 min; default 90 s; 0 = disabled) applied live to the autosave
      timer, driven from a compact "Session Recovery" group in the existing
      Audio Settings tab (Autosave toggle + Interval combo + Save Recovery
      Copy Now); autosave still writes a separate recovery file, never the
      project, with .prev rotation and atomic replacement, and is cleaned on
      normal save. Open Recent keep missing entries, visually marked
      "— unavailable", removed only after user confirmation; colliding file
      names get `~/…` path context; recent/projects persisted and capped 12;
      QA: interval set/persist/restore + page-to-timer wiring + menu marking/
      collision checks — 755 QA checks total)
  10 FLP import completion       COMPLETE     (#7 Parts 3-16: zlib-compressed
      ("zipped") FLdt payloads inflate and decode identically; every FL
      channel materializes as a track + channel-rack row (samplers carry the
      resolved sample in soundFile, generators become Instrument rows);
      FL-plugin channels keep their preserved names as unsupported placeholders
      and are listed in the import report; sample paths resolve against the
      .flp directory with found/missing counts and re-pointable rows; the
      busiest arrangement becomes the timeline with real per-note timing and
      tempo; imported projects render audible audio through the normal
      transport (QA plays a representative FLP headless, master peak > 0.01);
      import report dialog shows compact counts; importFlpFromPath() public
      entry sets dirty_, never adds a Recent entry until Save, .misuc
      reopen+play verified. 65 unit checks (55818) + 32 QA checks (787).)
  11 Playlist selection/tracks   COMPLETE     (#8: real selection model,
      dynamic tracks, multi-select ops, undo; all Tests A-L implemented)
  12 Playlist paint brush        COMPLETE     (#9: B tool, paint/erase,
      snap, one-gesture undo, ghost preview, Ctrl/Shift/Alt modifiers, ghost preview)
  13 Record Setup + real record  COMPLETE     (#10: Record Setup dialog with
      input device, format, destination, count-in, metronome, file name/path;
      dedicated recording transport state; real audio capture via PipeWire;
      WAV/FLAC file output; Playlist clip with waveform; save/reload/undo verified)
  14 Transport state             COMPLETE     (#11: Play button shows ▶/⏸
      synced to real transport state; click toggles play/pause; hint bar synced)
  15 Metronome redesign          COMPLETE     (#12: professional percussive click
      with fast attack/short decay (30ms), 2nd harmonic for definition; base 1500 Hz
      (normal) / 3000 Hz (accent) for clear distinction; dedicated volume control)
  16 Export dialog + codecs      COMPLETE     (#13: WAV float/PCM16, FLAC,
       MP3, OggVorbis, AIFF bounce through the real menu path and decode back;
       dialog offers exactly the probed codecs + correct per-codec suffix (Phase
       S); per-format dither gating; progress dialog that reports elapsed time
       and honours Cancel mid-render (no partial file); Include Metronome
       reaches the bounce even when the live click is off)
17 Native .misuc container     COMPLETE     (#14: real ZIP container with
       misuc.meta (format + containerVersion) + misuc.song payload, zlib
       deflate + CRC32 verification, atomic temp+rename writes, EOCD/central-
       dir parsing with bounds checks, seamless legacy plain-JSON migration
       on read and re-save as a container on the next write, truncated/
       bit-flipped corruption rejected. Tested: write/read round-trip,
       multi-entry asset payload (deflate forced), deflate + stored identical,
       CRC mismatch and truncation detection, missing file, and the live app
       flows (Save/Open/autosave rotate+verify/recovery/demo project all use
       the container via writeProjectDocument/readProjectDocument).)
  18 Global accent color         COMPLETE     (#15: accent drives all
      elements + logo; persistence; no flash. Tested: accent propagation,
      persistence round-trip via QSettings + .misuctheme JSON + file I/O,
      preset override, navLogo refresh, logo pixmap differs per accent.
      Fixed: loadPersisted now restores custom accents that stored
      "Custom" by tracking the underlying base preset.)
  19 Mixer overhaul              COMPLETE     (#16: routing graph,
      real meter/fader/fx/EQ/buses/sends/sidechain, export + save/load.
      Comp19: track→bus routing, per-track parallel sends to buses,
      SC bus compressor, BusFxDialog and SendDialog, engine RT with
      DAG topological fold, cycle-safe bus ordering, QA 24 checks.)

VERIFICATION POLICY
-------------------
  No component is COMPLETE from compilation. Every feature verified by:
  automated tests + QA harness + running the installed application.
  Audio features verified with actual rendered/played audio; persistence
  verified by save/reopen; packaging verified from the installed .deb.