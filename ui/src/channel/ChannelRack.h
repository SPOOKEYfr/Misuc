#pragma once

#include <QVector>
#include <QWidget>

class QStandardItemModel;
class QTableView;
class QToolButton;
class QAbstractButton;

namespace misuc::ui {

class HintManager;

// A real channel/step-sequencer model: named channels with per-channel
// mute/solo, volume (0..1), pan (-1..1) and a 16-step gate lane. Mutations
// are model-owned (editable table), persist across runs via QSettings, and are
// seeded with the classic starter kit (Kick/Snare/Hi-Hat/Bass/Sampler). The
// steps lane is painted by a custom delegate; clicking a step toggles it.
class ChannelRack : public QWidget {
  Q_OBJECT
public:
  struct Channel {
    QString name;
    bool muted = false;
    bool solo = false;
    float volume = 0.80f; // 0..1
    float pan = 0.0f;     // -1..1
    QVector<bool> steps;  // gate lane
  };

  explicit ChannelRack(QWidget* parent = nullptr);

  const QVector<Channel>& channels() const { return channels_; }
  int currentRow() const { return current_; }
  bool hasSelection() const { return current_ >= 0 && current_ < channels_.size(); }

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

signals:
  void edited();
  void channelSelected(int row);
  void previewRequested(int row);

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
  QTableView* view_ = nullptr;
  QStandardItemModel* model_ = nullptr;
  int stepBase_ = 3; // columns before the step grid
  QVector<QToolButton*> chromeButtons_;
  double previewPitch_[5] = {55.0, 220.0, 660.0, 55.0, 440.0};
};

} // namespace misuc::ui