#pragma once

#include <QVector>
#include <QWidget>

class QSlider;
class QStandardItemModel;
class QTableView;
class QToolButton;
class QAbstractButton;

namespace misuc::ui {

class HintManager;

// A real channel/step-sequencer model: named channels with per-channel
// mute/solo, volume (0..1), pan (-1..1) and a 16-step gate lane per pattern
// bank. Mutations are model-owned (editable table), persist across runs via
// QSettings, and are seeded with the classic starter kit
// (Kick/Snare/Hi-Hat/Bass/Sampler). The steps lane is painted by a custom
// delegate; clicking a step toggles it. Pattern banks (1..4) and the rack-wide
// swing amount are first-class controls: they switch every step field and
// persist, so ideas are kept in 16-step groups per pattern. The Pattern Play
// chrome toggle arms live sequencing: with it checked, MainWindow publishes
// the active pattern bank to the engine MIDI player as a looping 4-beat score,
// so every gated step auditions through the real audio pipeline (swing is
// realized as a start-beat offset on off-beat steps).
class ChannelRack : public QWidget {
  Q_OBJECT
public:
  static constexpr int kPatternCount = 4;
  static constexpr int kMaxSwing = 66; // percent, 0..66

  struct Channel {
    QString name;
    bool muted = false;
    bool solo = false;
    float volume = 0.80f; // 0..1
    float pan = 0.0f;     // -1..1
    // Gate lanes, one 16-step row per pattern bank.
    QVector<bool> steps[kPatternCount];
  };

  explicit ChannelRack(QWidget* parent = nullptr);

  const QVector<Channel>& channels() const { return channels_; }
  int currentRow() const { return current_; }
  bool hasSelection() const { return current_ >= 0 && current_ < channels_.size(); }
  int pattern() const { return pattern_; }
  int swing() const { return swing_; }
  // True while the Pattern Play chrome toggle is checked.
  bool patternLive() const;

  // Registers the icon-only chrome with the global hint manager.
  void attachHints(HintManager* hints);

  // Base pitch (Hz) used to audition the current channel through the engine.
  double previewFreq(int row) const;

  // Current channel's sound killer for the Inspector / mixer lanes.
  QString channelName(int row) const;

public slots:
  void addChannel(const QString& name = QString());
  void removeCurrent();
  void duplicateCurrent();
  void previewCurrent();
  void resetToDefaults();
  void setPattern(int pattern);
  void setSwing(int percent);
  // Capitol Tool: turns the active pattern bank's gated steps into a real
  // session MIDI region (named "Pattern N", 4 beats) that lands in the
  // arrangement and is then editable in the piano roll. The heavy lifting
  // lives in MainWindow (it owns the Session); this slot just announces it.
  void placePattern();
  // Programmatic step-sequencer editing (used by the human/MIDI programming
  // surface and the QA harness); each call republishes pattern playback when
  // it is armed. Missing rows/steps are ignored.
  bool setStep(int row, int bank, int step, bool on);
  void setMuted(int row, bool on);
  void setSolo(int row, bool on);
  void setVolume(int row, float v);

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
  void savePersisted();
  void loadPersisted();
  void setCurrent(int row);
  int columnOfStep(int logical) const; // step columns start after name/mute/solo

  QVector<Channel> channels_;
  int current_ = -1;
  int pattern_ = 0;      // active pattern bank (0..kPatternCount-1)
  int swing_ = 0;        // percent, 0..kMaxSwing
  QTableView* view_ = nullptr;
  QStandardItemModel* model_ = nullptr;
  QSlider* swingSlider_ = nullptr;
  int stepBase_ = 3; // columns before the step grid
  QVector<QToolButton*> chromeButtons_;
  QVector<QToolButton*> patternButtons_;
  QToolButton* patternPlayBtn_ = nullptr;
  QToolButton* placePatternBtn_ = nullptr;
  double previewPitch_[5] = {55.0, 220.0, 660.0, 55.0, 440.0};
};

} // namespace misuc::ui