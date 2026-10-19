#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

#include <misuc/engine/audio/AudioEngine.h>

class QLabel;
class QPushButton;
class QScrollArea;

namespace misuc::project {
class Bus;
class Session;
class Track;
}

namespace misuc::ui {

class ChannelStrip;
class Fader;
class VerticalMeter;
class HintManager;

// Real mixer panel: one strip per track (routing, pan, M/S, stereo meter,
// fader), one strip per named bus (a whole subgroup of routed tracks), plus a
// master bus strip. Strips mirror the Session; edits are reported up via
// edited() so the main window pushes an undo snapshot, and mixer params are
// published to the engine by syncToEngine(). Meters poll real signal.
class MixerPage : public QWidget {
  Q_OBJECT
public:
  explicit MixerPage(misuc::project::Session& session,
                     misuc::engine::AudioEngine& engine,
                     QWidget* parent = nullptr);

  // Rebuild strips to match the session (new/open/undo/redo/undoable edits).
  void refresh();
  // Lightweight mirror — updates strip values without recreating widgets, so a
  // fader drag or knob turn is never interrupted. Rebuilds if tracks changed.
  void syncFromSession();

  // Publish current session mixer + bus state to the engine audio thread.
  void syncToEngine();

  // 30 Hz meter + master poll from the main window timer.
  void pollMeters();

  // Select a strip by track id (from the editor's track list).
  void selectTrack(const QString& id);

  // Registers exact-control hints on every channel strip (and the master bus)
  // so the HintBar shows the live value under the cursor. Called once at shell
  // build; strips created later by rebuildStrips() self-register too.
  void attachHints(HintManager* hints);

signals:
  void edited();
  void trackSelected(const QString& id);

private:
  void rebuildStrips();
  void onStripActivated(const QString& id);
  void onPanChanged(const QString& id, int pan);
  void onGainChanged(const QString& id, float db);
  void onMuteToggled(const QString& id, bool on);
  void onSoloToggled(const QString& id, bool on);
  void onArmToggled(const QString& id, bool on);
  void onRouteChanged(const QString& id, int busIndex);
  void onBusRouteChanged(const QString& id, int busIndex);
  void onSendsRequested(const QString& id);
  void onFxRequested(const QString& id);
  void onMasterGainChanged(float db);
  void onAddBus();
  void onBusDeleted(const QString& id);
  void onBusPanChanged(const QString& id, int pan);
  void onBusGainChanged(const QString& id, float db);
  void onBusMuteToggled(const QString& id, bool on);
  void onBusSoloToggled(const QString& id, bool on);
  int trackIndex(const QString& id) const;
  int busIndex(const QString& busId) const;
  misuc::project::Bus* busById(const QString& id);
  QColor stripColor(size_t index) const;
  QColor busStripColor(size_t index) const;

  misuc::project::Session* session_;
  misuc::engine::AudioEngine& engine_;

  QScrollArea* scroll_ = nullptr;
  QWidget* stripsHost_ = nullptr;
  std::vector<ChannelStrip*> strips_;
  std::vector<ChannelStrip*> busStrips_;
  std::vector<QPushButton*> busDeleteBtns_;
  Fader* masterFader_ = nullptr;
  VerticalMeter* masterMeter_ = nullptr;
  QLabel* masterReadout_ = nullptr;
  QString selectedId_;
  bool syncing_ = false;
  HintManager* hints_ = nullptr;
};

} // namespace misuc::ui