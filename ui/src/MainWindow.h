#pragma once

#include <QMainWindow>
#include <QString>
#include <QList>
#include <QHash>
#include <QPointer>
#include <optional>

#include <misuc/engine/core/UndoStack.h>
#include <misuc/engine/project/Session.h>
#include <misuc/engine/audio/WavFile.h>
#include <misuc/engine/fx/Lv2Host.h>
#include <misuc/engine/flp/FlpImport.h>
#include "inspector/Inspector.h"
#include "workspace/WorkspaceManager.h"

class QAction;
class QTimer;
class QLabel;
class QTabWidget;
class QDockWidget;

namespace misuc::engine {
class AudioEngine;
}

namespace misuc::ui {

class HeaderBar;
class RecordSetupDialog;
class HintManager;
class StatusPanel;
class ProjectPage;
class EditorPage;
class AudioSettingsPage;
class MixerPage;
class ThemePage;
class TransportBar;
class Playlist;
class ChannelRack;
class Browser;
class Inspector;
class WorkspaceManager;

// Application shell: global header toolbar (brand + transport + status),
// a tabbed central workspace (Project / Editor / Audio / Appearance), a
// dockable Mixer panel, a contextual HintBar + status strip, and the File /
// Edit / Workspace / Help menus. Project document semantics are unchanged.
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(misuc::engine::AudioEngine& engine);
  ~MainWindow() override;

  // Document access for embedders / tests. Mutations should be followed by
  // sessionEdited() so the engine and undo stack pick up the change.
  misuc::project::Session& session() noexcept { return session_; }
  void sessionEdited() { onSessionEdited(); }
  // Test hook: skip the save dialog and render straight to this path.
  void setExportPathOverride(const QString& path) { exportPathOverride_ = path; }
  // Test hook: force export options for the override path (defaults: whole
  // song, WAV float, no dither -- identical to the old renderToWav contract).
  void setExportOptionsForTest(int format, bool loopOnly, bool dither) {
    exportOverrideOptions_ = true;
    exportOverrideFormat_ = format;
    exportOverrideLoop_ = loopOnly;
    exportOverrideDither_ = dither;
  }
  void clearExportOptionsForTest() { exportOverrideOptions_ = false; }
  // Test hook: force the exported sample rate for the override path (0 = the
  // engine's native rate). Distinct from setExportOptionsForTest so QA can
  // exercise the Phase R resampled bounce.
  void setExportSampleRateOverrideForTest(uint32_t sr) {
    exportOverrideSampleRate_ = sr;
  }
  // Test hook: include the metronome click in the headless override export
  // (protocol #13 §13 — the metronome renderer must be audible when asked).
  void setExportMetronomeForTest(bool on) { exportOverrideMetronome_ = on; }
  // Test hook: arm/disarm channel-rack pattern playback without the UI toggle.
  void setRackPatternLiveForTest(bool live) { onRackPatternLive(live); }
  // Drive one live per-lane automation record tick. Public so the offscreen
  // QA harness runs a capture synchronously (the production timer calls the
  // same private PollAutomationRecord each 20 ms).
  void recordAutomationTick() { pollAutomationRecord(); }
  // Test hooks: direct save/load with an explicit path (used by the harness
  // to verify the portable asset-path round-trip and app restore).
  bool writeProject(const QString& path);
  void openProject(const QString& path);

  // Test hook: import an .flp by path, no dialogs, no discard confirmation.
  // Adopts the parsed project as the live document (dirty, cleared project
  // path, fresh undo). `outResult` (optional) receives the structured import
  // report so the harness can assert on what landed. Returns false when the
  // file cannot be read/parsed (the session is then left untouched).
  bool importFlpFromPath(const QString& path,
                         misuc::flp::FlpImportResult* outResult = nullptr);

  // Autosave + crash recovery entry points (used by main() and the harness).
  static QString autosavePath();
  static QString autosavePrevPath();
  void autosaveNow();
  void checkAutosaveRecovery();
  // Loads `path` without treating it as the project document (keeps
  // projectPath_ empty so the next Save prompts for a real location).
  bool restoreSessionFrom(const QString& path, bool markDirty);

  // Configurable autosave interval (seconds; 0 = disabled). The setting is
  // persisted through QSettings and applied to the running timer immediately.
  int autosaveIntervalSeconds() const { return autosaveIntervalSecs_; }
  void setAutosaveIntervalSeconds(int seconds);

  // Recent-projects list (most recent first). The raw list keeps entries
  // whose files have since gone missing so the Open Recent menu can mark
  // them unavailable / offer removal.
  QStringList recentProjects() const;
  void addRecentProject(const QString& path);

  HeaderBar* headerBar() const { return headerBar_; }
  // Live project document (read-only) so the QA harness can verify edits that
  // flowed through the real widgets.
  const misuc::project::Session& session() const noexcept { return session_; }
  // Workspace/dock management (used by the offscreen QA harness to drive
  // preset switches, dock/undock, persistence and resize stress).
  WorkspaceManager* workspaceManager() const { return wsMan_; }

protected:
  void paintEvent(QPaintEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

private:
  void buildMenus();
  void buildPages();
  void buildCommands();
  void poll();
  void feedPlayhead();
  void onSessionEdited();
  void onInspectorEdit(const QString& property, const QString& value);
  void refreshInspector();
  void syncWorkspaceStrip();
  // Help -> About Misuc…
  void showAbout();
  // Help -> User Guide… (shipped tutorial text)
  void showUserGuide();
  void applyLayoutPreset(const QString& name);
  void persistLayout();
  void restoreLayout();
  // Keeps the View menu's layout-preset check mark in sync with the real
  // workspace state (a preset, or Custom after a manual rearrangement).
  void syncLayoutActions();
  // Re-checks the View menu's panel toggles against real dock/tab visibility
  // (dock panels: not hidden; central tabs: currently active page).
  void syncWsPanelActions();
  // View > panel visibility toggles (the ordered id each action belongs to).
  // QPointer so a late visibility signal during window teardown (menus already
  // destroyed) never dereferences a dangling action.
  QVector<QPair<workspaces::Id, QPointer<QAction>>> wsPanelActions_;
  // Populated by buildMenus(); the checkable panel toggles are added here
  // (invoked once the workspace manager exists) because building them touches
  // real dock/tab state.
  void buildWorkspaceViewMenu();
  QMenu* viewMenu_ = nullptr;

  void newProject();
  void openDialog();
  void openDemoProject();
  bool saveProject();
  bool saveProjectAs();
  void exportAudio();
  void importFlp();
  // Renders `path` from the engine with the given container/range/dither and
  // restores the live stream afterwards (the transport may be paused while
  // bouncing). Shared by File > Export and the headless override. `title`/
  // `artist` become the file tags; `exportSampleRate` (0 = engine native)
  // selects the export frame rate (Phase R); `includeMetronome` bounces the
  // metronome click into the file.
  void doExport(const QString& path,
                misuc::audio::ExportFormat format,
                int rangeKind, bool dither, const QString& title = {},
                const QString& artist = {}, uint32_t exportSampleRate = 0,
                bool includeMetronome = false);
  void performUndo();
  void performRedo();
  void togglePlayStop();
  void applyTempoToEngine();
  void applyLoopToEngine(misuc::project::Session& s);
  void applyContentEndToEngine(); // publishes session_.contentEndBeats()
  // Pushes the session's metronome on/volume/count-in settings to the
  // transport bar and engine (phase 17 metronome pass).
  void applyMetronomeToEngine();
  // Real transport loop toggle: writes the loop into the Session (so it is
  // persisted and undoable, not just a transient engine/UI state) and keeps
  // the engine, transport bar and piano-roll ruler in sync.
  void onLoopToggled(bool on);
  // Pushes the Session's persisted loop region into the engine, transport bar
  // and piano-roll ruler after load/undo/redo/new, always loopStart < loopEnd.
  void syncLoopState();
  // Publishes the session's per-track FX inserts to the audio engine so the
  // post-fader chains are real during playback (Phase 17 LV2 hosting). The
  // chain index is the session track index; tracks without inserts publish an
  // empty chain.
  void applyFxToEngine();
  // View models for the Inspector FX rack: the resolved slot list (names and
  // param ranges from the discovered catalog) and the catalog itself.
  std::vector<misuc::ui::FxSlotView> buildFxSlots(const misuc::project::Track& tr);
  std::vector<misuc::ui::FxCatalogItem> buildFxCatalog();
  void rebuildMidiScore();
  void rebuildClipScore();
  // Publishes every track's automation lanes (gain/volume/pan) to the audio
  // engine so curves are audible during playback and offline export (§24
  // automation; Phase L expanded the gain-only publish to all params).
  void applyAutomationToEngine();
  // Snapshot latch for pollAutomationRecord: set when a recording pass has
  // already pushed its one undo state; cleared when the transport stops.
  bool autoRecUndoLatch_ = false;
  // Live per-lane automation record: while the transport runs and a lane is
  // armed, sample the tracked control value at the transport position and
  // write automation points. Runs on a short timer; the first written point
  // of a pass snapshots the Session into undo so the whole pass undoes once.
  void pollAutomationRecord();
  // Phase Q: a record-armed lane being dragged on the Playlist band taps the
  // dragged point's value at the transport position into bandTape_; when the
  // drag ends the tape is merged into the lane right before the grid's
  // edited() (one undo step covers drag + tape).
  void onBandDragFinished(int row, int autoIdx);
  // Captured band-drag edits awaiting merge at drag release (Phase Q).
  std::vector<misuc::project::AutomationPoint> bandTape_;
  // Channel-rack pattern playback: publishes the armed pattern bank as a
  // looping 4-beat engine score so steps audition the real audio pipeline.
  void onRackPatternLive(bool live);
  void rebuildRackScore();
  void onRecordRequested();
  void onRecordStopped();
  void onRecordToggled(bool on);
  // Relocate an imported or recorded audio file into the project's audio/
  // directory when the project has a home; otherwise keep the source path.
  QString stageAudioFile(const QString& src) const;
  void updateWindowTitle();

  // Ask about unsaved changes. Returns false when the user cancelled.
  bool confirmDiscard();
  void clearAutosave();
  void rebuildRecentMenu();
  QStringList recentProjectsFromSettings() const;
  void applyAutosaveInterval(int seconds);

  misuc::engine::AudioEngine& engine_;
  // Decoded rack sampler sounds (one copy per dropped file), reused by every
  // pattern-live republish so toggling a step never re-decodes the asset.
  QHash<QString, misuc::audio::AudioBuffer> rackSampleCache_;
  misuc::project::Session session_;
  misuc::core::UndoStack<misuc::project::Session> undo_;
  QString projectPath_;
  bool dirty_ = false;
  // Last FX chains published to the engine. applyFxToEngine() re-publishes
  // only when the chains actually changed, so unrelated edits (moving a note,
  // tweaking a mixer gain) do not re-instantiate (and thus do not reset) the
  // effect instances mid-playback.
  std::vector<std::vector<misuc::engine::FxInsertParam>> lastFxChains_;
  // Per-bus chains alongside the track chains (Comp19); compared together so a
  // bus-insert edit alone re-publishes rather than being mistaken for a no-op.
  std::vector<std::vector<misuc::engine::FxInsertParam>> lastBusFxChains_;
  // True while the channel-rack stepper owns the engine playback score.
  bool rackPatternLive_ = false;

  HeaderBar* headerBar_ = nullptr;
  TransportBar* transport_ = nullptr;
  StatusPanel* status_ = nullptr;
  HintManager* hints_ = nullptr;
  ProjectPage* projectPage_ = nullptr;
  EditorPage* editorPage_ = nullptr;
  AudioSettingsPage* audioSettings_ = nullptr;
  MixerPage* mixerPage_ = nullptr;
  ThemePage* themePage_ = nullptr;
  Playlist* playlist_ = nullptr;
  ChannelRack* channelRack_ = nullptr;
  Browser* browser_ = nullptr;
  Inspector* inspector_ = nullptr;
  WorkspaceManager* wsMan_ = nullptr;
  QTimer* timer_ = nullptr;
  QTimer* autosaveTimer_ = nullptr;
  int autosaveIntervalSecs_ = 90; // seconds; 0 = disabled
  QTimer* automationRecTimer_ = nullptr;
  QTabWidget* workspace_ = nullptr;
  QDockWidget* mixerDock_ = nullptr;

  // Headless/testing hook: when set, exportAudio() skips the save dialog and
  // renders directly to this path.
  QString exportPathOverride_;
  // Optional forced export options for the override path. When unset, the
  // override bounces the whole song as WAV float (the old renderToWav
  // behaviour). Range kind: 0 whole / 1 loop / 2 selection.
  bool exportOverrideOptions_ = false;
  int exportOverrideFormat_ = 0; // misuc::audio::ExportFormat as int
  bool exportOverrideLoop_ = false;
  bool exportOverrideDither_ = false;
  uint32_t exportOverrideSampleRate_ = 0; // 0 = native (Phase R)
  bool exportOverrideMetronome_ = false;   // include metronome click in bounce

  // Beat position where recording started (kept for any external readers; the
  // placed clip now uses the engine's own recorded span, which is accurate
  // across count-in). The clip counter names staged WAV assets.
  double recordStartBeats_ = 0.0;
  int recordCounter_ = 0;

  struct RecordConfig {
    enum class Type { Audio, Midi } type = Type::Audio;
    std::string inputDeviceId;
    std::string inputDeviceName;
    int inputChannels = 2;
    uint32_t sampleRate = 48000;
    std::string format = "wav";
    std::string recordPath;
    std::string fileName;
    int countInBars = 2;
    bool metronomeOn = true;
    int destinationTrack = -1;
    bool createNewTrack = true;
    bool inputMonitor = true;
  };
  std::optional<RecordConfig> currentRecordConfig_;

  // Phase G recording helpers.
  std::string recordTargetTrack() const; // first armed track, else plantation
  bool anyRecordArmed() const;           // drives engine input monitoring
  // Collapses the timeline on the given track to exactly this take: every clip
  // that materialized a session take is dropped and this take's audio is
  // re-placed at its singer span.
  void placeTake(misuc::project::Track& tr, misuc::project::RecordTake& take);
  std::vector<misuc::ui::TakeView> buildTakeViews(
      const misuc::project::Track& tr);

  QAction* undoCmd_ = nullptr;
  QAction* redoCmd_ = nullptr;
  QAction* snapCmd_ = nullptr;
  QAction* modeCmd_ = nullptr;   // song/pattern
  QAction* metronomeCmd_ = nullptr;
  QMenu* recentMenu_ = nullptr;  // File > Open Recent (rebuilt on demand)
  QList<QAction*> presetActions_; // View > layout-preset menu entries
  QString inspectorClipId_;
  int inspectorTrackRow_ = -1;
  bool inspectorOnClip_ = false;
};

} // namespace misuc::ui