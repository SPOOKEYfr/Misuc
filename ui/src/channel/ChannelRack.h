#pragma once

#include <QEvent>
#include <QVector>
#include <QWidget>

#include <misuc/engine/project/Session.h>

class QSlider;
class QStandardItemModel;
class QTableView;
class QToolButton;
class QComboBox;
class QAbstractButton;

namespace misuc::ui {

class HintManager;

// A real channel/step-sequencer model: named channels with per-channel
// mute/solo, volume (0..1), pan (-1..1) and a 16-step gate lane per pattern
// bank. Mutations are model-owned (editable table), persist via the Session
// (the rack is a first-class element of the .misuc document), and are seeded
// with the classic starter kit (Kick/Snare/Hi-Hat/Bass/Sampler). The steps
// lane is painted by a custom delegate; clicking a step toggles it. Pattern
// banks (1..4) and the rack-wide swing amount are first-class controls: they
// switch every step field and persist, so ideas are kept in 16-step groups
// per pattern. The Pattern Play chrome toggle arms live sequencing: with it
// checked, MainWindow publishes the active pattern bank to the engine MIDI
// player as a looping 4-beat score, so every gated step auditions through the
// real audio pipeline (swing is realized as a start-beat offset on off-beat
// steps).
class ChannelRack : public QWidget {
  Q_OBJECT
public:
  static constexpr int kPatternCount = 4;
  static constexpr int kMaxSwing = 66; // percent, 0..66
  // Velocity a new "on" step gets (matches the engine's default).
  static constexpr quint8 kDefaultStepVelocity = 100;

  struct Channel {
    QString id;   // stable identity, distinct from the editable name
    QString name;
    misuc::project::ChannelType type = misuc::project::ChannelType::Sampler;
    bool muted = false;
    bool solo = false;
    float volume = 0.80f; // 0..1
    float pan = 0.0f;     // -1..1
    // Sound asset for a sampler channel (audio file its steps trigger).
    // Empty for the built-in instrument voices. Cross-checked with the
    // Session's StepChannel so a dropped browser sample's reference survives
    // edits and save/reload.
    QString soundFile;
    // Gate lanes, one step row per pattern bank. 0 = off, 1..127 = on with
    // that velocity (mirrors the engine StepChannel model exactly).
    QVector<quint8> steps[kPatternCount];
  };

  explicit ChannelRack(misuc::project::Session& session, QWidget* parent = nullptr);

  const QVector<Channel>& channels() const { return channels_; }
  int currentRow() const { return current_; }
  bool hasSelection() const { return current_ >= 0 && current_ < channels_.size(); }
  int pattern() const { return pattern_; }
  int swing() const { return swing_; }
  // Pattern length in steps (4..128). The rack is NOT hardcoded to 16 steps
  // (master spec §9): the step grid renders exactly this many columns and the
  // Session persists the count per rack.
  int stepsPerPattern() const { return stepsPerPattern_; }
  // True while the Pattern Play chrome toggle is checked.
  bool patternLive() const;

  // Re-reads the channel rack from the bound Session (used after open/new and
  // after undo/redo, when MainWindow swaps the session document in place).
  void refresh();

  // Registers the icon-only chrome with the global hint manager.
  void attachHints(HintManager* hints);

  // Base pitch (Hz) used to audition the current channel through the engine.
  double previewFreq(int row) const;

  // Current channel's sound killer for the Inspector / mixer lanes.
  QString channelName(int row) const;

public slots:
  void addChannel(const QString& name = QString());
  // Creates a new Sampler channel wired to the given audio file (the Browser
  // → Channel Rack drop): named from the file stem, immediately usable, and
  // its sound reference persisted with the Session (master spec §4).
  void addSamplerChannelFromSound(const QString& path);
  void removeCurrent();
  void duplicateCurrent();
  void previewCurrent();
  void resetToDefaults();
  void setPattern(int pattern);
  void setSwing(int percent);
  // Resizes every channel's step lane to `steps` (4..128): the grid rebuilds
  // with that many step columns, the Session's stepsPerPattern follows, and
  // the change lands in one undo snapshot.
  void setStepsPerPattern(int steps);
  // Reorder the selected channel by one position (Alt+Up / Alt+Down on the
  // grid). Model + Session stay in lock-step; one undo snapshot each.
  void moveUp();
  void moveDown();
  // Capitol Tool: turns the active pattern bank's gated steps into a real
  // session MIDI region (named "Pattern N", 4 beats) that lands in the
  // arrangement and is then editable in the piano roll. The heavy lifting
  // lives in MainWindow (it owns the Session); this slot just announces it.
  void placePattern();
  // Programmatic step-sequencer editing (used by the human/MIDI programming
  // surface and the QA harness); each call republishes pattern playback when
  // it is armed. Missing rows/steps are ignored.
  bool setStep(int row, int bank, int step, bool on);
  // Per-step velocity: 0 removes the step, 1..127 sets its volume. Ends in
  // one undo snapshot and republishes pattern playback when armed.
  void setStepVelocity(int row, int bank, int step, quint8 vel);
  quint8 stepVelocity(int row, int bank, int step) const;
  void setMuted(int row, bool on);
  void setSolo(int row, bool on);
  void setVolume(int row, float v);
  void setPan(int row, float p);
  // Per-channel type (sampler/instrument/plugin/audio clip/...): a persisted
  // first-class property. The badge shows in the row's leading column and the
  // stable channel id (separate from the name) is exposed for tooltips.
  void setChannelType(int row, misuc::project::ChannelType type);
  misuc::project::ChannelType channelType(int row) const;
  QString channelId(int row) const;
  // Sound asset path on a channel (empty for built-in voices).
  QString channelSoundFile(int row) const;

  // Writes the current rack state into the bound Session. Called BEFORE
  // edited() fires, so the undo stack always snapshots the post-edit document.
  void syncToSession();

signals:
  void edited();
  void channelSelected(int row);
  void previewRequested(int row);
  // The user armed pattern playback: MainWindow publishes this rack's active
  // pattern to the engine MIDI player so steps audition the real audio path.
  void patternLiveChanged(bool live);
  // The user clicked Place Pattern: MainWindow materializes the active bank
  // into a real session MIDI region on a (new) track.
  void placePatternRequested();

protected:
  void restoreDefaults();

private:
  void rebuildTable();
  void syncFromSession();  // bound Session → channels_/pattern_/swing_
  void setCurrent(int row);
  int columnOfStep(int logical) const; // step columns start after name/mute/solo

  bool eventFilter(QObject* watched, QEvent* event) override;

  misuc::project::Session& s_;
  QVector<Channel> channels_;
  int current_ = -1;
  int pattern_ = 0;      // active pattern bank (0..kPatternCount-1)
  int swing_ = 0;        // percent, 0..kMaxSwing
  int stepsPerPattern_ = 16; // 4..128, persisted with the rack
  QTableView* view_ = nullptr;
  QStandardItemModel* model_ = nullptr;
  QSlider* swingSlider_ = nullptr;
  int stepBase_ = 4; // columns before the step grid
      // (0=Type, 1=name, 2=mute, 3=solo)
  QVector<QToolButton*> chromeButtons_;
  QVector<QToolButton*> patternButtons_;
  QToolButton* patternPlayBtn_ = nullptr;
  QToolButton* placePatternBtn_ = nullptr;
  QComboBox* stepsCombo_ = nullptr;
};

} // namespace misuc::ui