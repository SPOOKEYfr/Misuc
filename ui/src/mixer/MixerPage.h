#pragma once

#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

#include <misuc/engine/audio/AudioEngine.h>

class QLabel;
class QScrollArea;

namespace misuc::project {
class Session;
class Track;
}

namespace misuc::ui {

class ChannelStrip;
class Fader;
class VerticalMeter;

// Real mixer panel: one strip per track (pan, M/S, stereo meter, fader) plus a
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

  // Publish current session mixer state to the engine audio thread.
  void syncToEngine();

  // 30 Hz meter + master poll from the main window timer.
  void pollMeters();

  // Select a strip by track id (from the editor's track list).
  void selectTrack(const QString& id);

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
  void onMasterGainChanged(float db);
  int trackIndex(const QString& id) const;
  QColor stripColor(size_t index) const;

  misuc::project::Session* session_;
  misuc::engine::AudioEngine& engine_;

  QScrollArea* scroll_ = nullptr;
  QWidget* stripsHost_ = nullptr;
  std::vector<ChannelStrip*> strips_;
  Fader* masterFader_ = nullptr;
  VerticalMeter* masterMeter_ = nullptr;
  QLabel* masterReadout_ = nullptr;
  QString selectedId_;
  bool syncing_ = false;
};

} // namespace misuc::ui