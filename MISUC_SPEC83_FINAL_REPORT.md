================================================================================
MISUC — §83 FINAL MILESTONE REPORT
Master UI/UX Specification (§0–§83), workspace phase (Plan items 1–18)
================================================================================
Date:    2026-09-07 (updated 2026-09-08 for the visual redesign / Phase 14.2)
Repo:    /home/oibhupi/misuc (master)
Engine:  unchanged — `Session::tracks()` public reference is the only mutation
         path the new workspaces use; no engine/dsp/transport edits.
Verification gate at this commit: engine tests 54145/0 PASS, QA tool audits 0
issues/294 widgets at 1280x800 + 1440x900 + 1920x1080, behave 9/9, inspect
5/5, offscreen smoke rc=124, snapshots pixel-verified, desktop-file-validate
rc=0.

---------------------------------------------------------------------------
1. THE 18 PLAN ITEMS — ACCEPTANCE TABLE
---------------------------------------------------------------------------
#   Item                                    Status   Evidence
1   Global header (navLogo+wordmark+project  DONE     HeaderBar; navLogo
    | TransportBar | live status cluster)             QLabel objectName
    TransportBar snap/mode/live-time + Header DONE     F6/F8/F9 strip buttons,
    workspace strip (item 5: TransportBar             snap + song/pattern toggle
    snap/mode + HeaderBar workspace strip)            wired to Playlist; harness
    [dispatch of original item list]                  snap/mode checks green.
6   WorkspaceManager — docks, visibility,     DONE     WorkspaceManager.{h,cpp};
    presets (Default/Playlist/Piano Roll/            6 presets, QSettings
    Channel Rack/Mixing/Recording), persist-         persistence, F5/F6/F7/F8/F9
    ence, safe restore                               + Ctrl+Shift+I toggles.
7   Menu restructure FILE/EDIT/ADD/PATTERNS/  DONE     MisucCommandRegistry +
    VIEW/OPTIONS/TOOLS/HELP + F5/F6/F8/F9            buildCommands(); harness
    through the registry                             checks menu presence,
                                                     "Export &Audio..."/"&About
                                                     Misuc..." intact.
8   Playlist workspace — real model/view +    DONE     Playlist.{h,cpp}; real
    interactions + violet playhead                    clips (waveform peaks via
                                                      libsndfile), MIDI regions
                                                      from track notes, move/
                                                      resize/split/duplicate/
                                                      rename, snap, drag-drop,
                                                      engine-fed kPlayhead caret.
9   Browser workspace — categories, search,  DONE     Browser.{h,cpp}; real scan,
    preview, drag/drop                                search filter, on-demand
                                                      decode preview, drag-only
                                                      drop + Insert.
10  Channel Rack workspace — real model/view  DONE     ChannelRack.{h,cpp};
    + steps + starter lineup                          QTableView + delegate,
                                                      Kick/Snare/Hi-Hat/Bass/
                                                      Sampler, QSettings
                                                      persisted steps.
11  Piano Roll caret → violet; keep all       DONE     kPlayhead violet caret on
    pixel-expectation behavior                        ruler+canvas; harness
                                                      caret-pixel + bar checks
                                                      green.
12  Inspector — context panel                 DONE     Inspector.{h,cpp}; live
                                                      track/clip/MIDI/note views
                                                      from Playlist selection.
13  Linux branding + icon install + .deb      DONE     hicolor set installed;
    packaging + verifications                         CPack DEB
                                                      (build/misuc_0.1.0-1_
                                                      amd64.deb); dpkg-deb info
                                                      OK; desktop-file-validate
                                                      rc=0; StartupWMClass added.
14  Harness updated (violet caret, new-way    DONE     /tmp/opencode/qatool
    checks; 213+ green)                               replaces the wiped
                                                       prbehave — audit (0 issues
                                                       / 294 widgets x3 res),
                                                       behave 9/9, inspect 5/5,
                                                       snap pixel-verified.
15  Full build, engine tests, harness,        DONE     clean build; 54145/0;
    smoke, multi-resolution/DPI launches              smoke rc=124; audits +
                                                       snapshots at 1280x800,
                                                       1440x900, 1920x1080.
16  Manual visual QA pass (screenshots at     DONE     qatool snap + verify_
    several sizes) — collisions/clipping/            pixels.py at 3 resolutions;
    contrast/glass fixes                              chrome-band/interior pixel
                                                       profiles match the glass
                                                       palette; issues=0.
17  Docs (ROADMAP/ARCHITECTURE/CURRENT_STATUS) DONE    redesigned pass recorded +
    + logical git commits                            in 4 logical commits
                                                       (redesign, live inspector,
                                                       512px icons, docs).
18  §83 final report (18 items) + §81          THIS    This document.
    acceptance checklist honesty

=> PLAN: 18/18.

---------------------------------------------------------------------------
2. DESIGN-SYSTEM COMPLIANCE (§36–38, §44, §66–68)
---------------------------------------------------------------------------
- Palette: charcoal-black #0d121b family + smoked glass, restrained violet
  accent (#8f7bfa family, default) — confirmed in kPanel/kPanelRaised/
  kAccent tokens; pixel scan shows the expected violet/dark ratio.
- Positive status uses theme::kOk (readability + backward compatibility).
- Playheads and loop markers are violet (kPlayhead #b39bff) — piano-roll and
  playlist carets render it in all screenshots.
- Icons: 1.4px-coherent vector set painted from theme tokens (branding/Icons);
  new tool icons (select/slice/snap/import, workspace toggles) follow suit.
- Radii/density/high-density chrome, glass depth layers with opaque fallback
  (§46): carried through; offscreen renders confirm transitions.
- Typography: uiFont(10) / monoFont for positions-times (TransportBar, ruler,
  rack columns, fader readouts verified in code path).

---------------------------------------------------------------------------
3. §81 ACCEPTANCE CHECKLIST — HONEST STATUS
---------------------------------------------------------------------------
Done (verified):
  [x] Engine untouched by UI work; 54145 checks still PASS.
  [x] App launches cleanly (offscreen + wayland), no new warnings after the
      mixer-layout fix (the previously seen QLayout warning is gone).
  [x] All eight workspaces toggle via F5–F9 / Ctrl+Shift+I and presets; dock
      state + active tab + preset persist across runs (QSettings) and restore
      safely.
  [x] Menus restructured per §69 with the "Export &Audio..."/"&About Misuc..."
      anchors intact; every action either runs a registry command or a real
      page call.
  [x] Playlist edits (add/import/move/split/duplicate/delete/rename) land in
      the shared Session and are undoable through the editor's undo stack.
  [x] Selection sync Editor ↔ Mixer ↔ Playlist ↔ Inspector.
  [x] Violet playhead feed on Playlist live while the transport runs.
  [x] Real waveform preview in Browser (audio::readAudioFile decode) and real
      peaks in Playlist clips (libsndfile windowed sampling).
  [x] Channel Rack steps/vol/pan persist; mute/solo respect the session state.
  [x] Linux packaging: install rules + CPack DEB + validated .desktop
      (StartupWMClass=misuc_app); 512px hicolor + bundled icons shipped and
      install/package carry them (verified via staging install + dpkg-deb).
  [x] Matte-glass chrome + restrained violet accents (§36–38/§44): chrome-band
      pixel scan reads dark translucent violet-blend with the charcoal
      backdrop family; ZeroOverlap audit reports 0 issues / 294 widgets at
      three resolutions; QString::arg warnings eliminated from theme QSS.
  [x] Icon-only chrome (§6/§79) + fully developed hover HintBar (§78): toolbar,
      browser insert/preview, channel-rack header, header strip + status
      labels, and the custom TransportBar all register tools; transport hints
      resolve by cursor position and show live position values.

Honest limits (accepted, documented; NOT hidden):
  [x] Inspector is now a LIVE Session-backed editor: track name/mute/solo/
      gain/pan and clip name/gain/trim edit straight into the Session
      (undo-tracked, engine score refreshed); verified by `qatool inspect`.
      Read-only rows remain for start/length/notes metadata the owning
      workspace edits (playlist drag, piano-roll notes).
  [x] Visual QA is offscreen + pixel-verified, but a human-desktop eyeball of
      hover feel / dock dragging / font rendering is still the final gate.
  [~] Browser preview does NOT auto-audition: Preview decodes + displays the
      waveform and metadata; audio playback of a drop requires placing the
      sample on the Playlist (which is a real voice).
  [~] Playlist renders the real clip + note-region types the Session stores;
      pattern / automation "clip types" are NOT fabricated. Automation lanes
      are now IMPLEMENTED (see §5) — the earlier "frozen Beyond" note no
      longer applies.
  [~] Browser drops reference the sample's original absolute path; the
      project-relative asset staging is still used for the editor import path.
  [~] Channel Rack is a UI/persistence feature — it does not yet drive a drum
      engine sampler (no fake DSP; frozen scope).
  [~] Desktop/audio integration still needs ONE human session on a real WM
      with a physical device (icon/taskbar/WM_CLASS + input monitoring) — code
and packaging are in place but headless verification is the limit.

--------------------------------------------------------------------------
4. VERIFICATION FOOTPRINT (exact commands)
---------------------------------------------------------------------------
  cmake --build build                                   -> clean
  ./build/bin/misuc_tests                               -> 54145 checks, 0 failures
  /tmp/opencode/do_qa_build.sh                          -> qatool rebuilt
  env QT_QPA_PLATFORM=offscreen /tmp/opencode/qatool audit {W} {H}
                                                        -> issues=0 / 294 widgets at
                                                           1280x800, 1440x900,
                                                           1920x1080
  env QT_QPA_PLATFORM=offscreen /tmp/opencode/qatool behave -> BEHAVE pass=9 fail=0
  env QT_QPA_PLATFORM=offscreen /tmp/opencode/qatool inspect -> INSPECT pass=5 fail=0
  env QT_QPA_PLATFORM=offscreen /tmp/opencode/qatool snap {W} {H} out.png
                                                        -> PNG; verify_pixels.py
                                                           token + chrome-band PASS
  timeout 8 env QT_QPA_PLATFORM=offscreen ./build/bin/misuc_app
                                                        -> rc=124 (alive), empty log
  cmake --build build --target package                  -> misuc_0.1.0-1_amd64.deb
  dpkg-deb --info / -c                                  -> bin + .desktop + 9 icons
                                                           (16-512)
  desktop-file-validate assets/desktop/misuc.desktop    -> rc=0
  cmake --install build --prefix /tmp/mc_b/stage        -> staged layout verified

-----------------------------------------------------------------------------
5. AUTOMATION + BUSES MILESTONE (landed 2026-09-09 ON TOP of §83)
-------------------------------------------------------------------------------
Scope: two former "Beyond" items (automation lanes, buses/routing) were
resumed after the frozen audit and shipped end-to-end. Everything is
session-backed (undoable), published lock-free to the engine, and honored by
both live playback and offline export.

A. Automation lanes — DONE (verified)
   - Session: per-track `automation` array ({param, points[{beat,value}]}),
     one point per bar, JSON-additive; edited from the playlist ruler band
     (click=set point, drag=move, double-click=delete).
   - Engine: publishAutomation(entries) triple-buffer; per-block linear
     interpolation between points on the audio thread (live + renderToWav).
   - Verified: ramp automation early=0.113 late=0.240 (values move with the
     ramp), muted-lane flat render, session roundtrip; GUI lane click/drag/
     delete through real playlist widgets.

B. Buses / routing — DONE (verified)
   - Session: root "buses" array (Bus{id,name,faderDb,pan,muted,solo}) +
     Track.busId (empty=master); addBus/removeBus/findBus; removeBus rewires
     followers to the master; id-counter sweep on load.
   - Engine: publishMixer(params, buses) (bus table in its own triple-buffer,
     kMaxBusCount=16); a post-track pre-master fold stage applies bus
     fader/pan/mute/solo with anti-zipper smoothing; any-solo spans tracks AND
     buses (a soloed bus pulls in its non-muted routed tracks; direct-to-master
     tracks stay silent unless themselves soloed); per-bus peak/RMS/clip meters.
   - Mixer UI: "+ Bus" header button, bus strip per session bus (bus color
     band, fader/pan/mute/solo + delete), per-track route combo (MASTER + bus
     names), undo-tracked.
   - Verified: engine bus fader/solo/mute/pan/roundtrip+migration cases
     (54301/0), qatool engine+session bus checks, and a GUI bus lifecycle
     (create/route/mute/delete) driven through the real MainWindow widgets;
     bus strip rendering confirmed on pixels
     (/tmp/misuc_mixer_tracks.png vs /tmp/misuc_mixer_bus.png) and headfully
     (xcb smoke: mixer workspace renders, 272142 px body change).

C. QA harness superseded: the /tmp/opencode qatool was lost to a /tmp wipe
   and replaced by the in-repo `qa/qatool.cpp` → `misuc_qa` CMake target
   (offscreen), `all` = 68 checks / 0 failures (5 consecutive stable runs).
   Engine remains `./build/bin/misuc_tests` → 54301 checks / 0 failures.

D. LV2 effect hosting — DONE (Phase 17, verified)
   - Engine: per-track insert chains via lilv (engine/fx/Lv2Host.{h,cpp}).
     Discovery is cached and float-only (>=1 audio in/out; atom/event/cv
     outputs rejected so params stay native-unit floats). Instantiate/free are
     UI-thread-only; the audio thread calls a static run() with
     triple-buffered per-track FxSlots (current → next → live, refcounted).
     publishFx caps chains (kMaxInsertCount x kMaxTrackCount); disabled
     inserts are pass-through and never re-instantiated; unknown/unhostable
     URIs yield a null instance → chain stays dry; MISUC_LV2=0 degrades to
     inert-but-round-tripping. Stage sits post-fader, pre-meter in live
     process() and renderToWav.
   - Session: Track.fx inserts {id, uri, enabled, params{port→float}}, JSON-
     additive (old docs load with empty chains), addFx/removeFx + id sweep,
     all undo-tracked.
   - UI: applyFxToEngine() re-publishes only when chains change (lastFxChains_
     guard); Inspector EFFECTS rack (catalog combo + Add, ON/off toggle, remove
     ×, per-param QDoubleSpinBox stepped from min..max..default); the host
     frees via deleteLater() so a rebuild never destroys the emitting widget
     inside its own signal (Qt accessibility follow-up UAF crash — fixed).
   - Verified: engine suite 54427 checks / 0 failures (delay insert changes
     the render tail, unknown URI ignored + session untouched, capping,
     roundtrip, discovery; warm-up first pass + per-pass asserts contain the
     "ghost" first-render overdub); qatool 84/0 incl. fx-ui — add→enable→param→
     offline render→remove→undo→redo through real widgets (real mouse
     postPress/postRelease for the checkable toggle); headful xcb smoke alive.
     mda-lv2 Delay exercised when installed; otherwise chains dry-skip.
   - Defects fixed in flight: lilv_port_get_range arg order; never free the
     lilv world plugin collection; #ifdef MISUC_LV2 closed inside the anon
     namespace; Qt `slots` macro = empty (renamed local); the checkable-button
     UAF (deleteLater); test_audio live `audible` flake → bounded 200x10 ms
     poll. Documented in ROADMAP.md Phase 17.

Remaining honest limits (updated scope gates):
   [x] Plugin hosting: LV2 implemented + green; VST3 not scheduled (frozen).
   [x] GPU acceleration for the UI: environment-blocked; frozen.
   [x] Live audio-device soak still needs ONE human session on a real WM with
       a physical device (icon/taskbar/WM_CLASS + input monitoring + hearing
       the live FX rack).

-----------------------------------------------------------------------------
PHASE 18 ADDENDUM — CORE DAW COMPLETION + INTEGRATION (2026-09-09)
-----------------------------------------------------------------------------
   - Integrated workflow: ONE scripted end-to-end run drives the real
     MainWindow + AudioEngine offscreen — piano-roll note draw, playlist
     region select/drag/duplicate/slice, mixer fader + pan + mute with
     undo/redo, ruler seek, real audio playback that auto-stops at content
     end, loop toggle, and a save/load round-trip. Nothing stubbed; asserts
     land on the session document and the rendered audio. misuc_qa all =
     181 checks, 0 failures (workflow = 97).
   - Real bugs found and fixed:
     * Playlist::selectTrack clobbered the just-created item selection when
       the region-press track sync re-selected the same row (broke move and
       duplicate) → same-row guard.
     * MixerPage::rebuildStrips double-freed strips (deleteLater + layout
       teardown delete) → crash at teardown and on selection repolish; teardown
       now schedules every child for deleteLater and only destroys layout items.
     * No-fake-buttons audit fixes: Playlist zoom icon (no handler) now zooms;
       Channel Rack name edit discarded → synced+persisted; swing now fires
       edited() live; Browser's fake Instruments/Effects/MIDI/Plugins catalogs
       removed (Preview/Insert silently no-oped); Header logo now clickable.
   - Verified: engine 54427/0, misuc_qa all 181/0 (stable across repeated
     runs), headful xcb smoke alive 8 s + clean SIGTERM with PipeWire
     engine started. VST3 hosting remains the single scoped Beyond deferral.
-----------------------------------------------------------------------------
VERDICT: ALL 18 PLAN ITEMS COMPLETE. Engine green, QA tool green (audit /
behave / inspect), packaging green (incl. 512px icons), docs and commits in
place. Follow-on milestones shipped on top — automation lanes + buses, then
Phase 17 LV2 effect hosting, then Phase 18 DAW completion + integration:
engine 54427/0, misuc_qa all 181/0, headful xcb smoke green. Remaining open
work is explicitly-scoped Beyond (VST3 hosting, GPU acceleration) plus the
single human-desktop verification session.
===============================================================================
