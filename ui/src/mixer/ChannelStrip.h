#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace misuc::ui {

class Fader;
class Knob;
class VerticalMeter;

// One mixer channel: name + color dot, pan knob, mute/solo, stereo meter,
// fader, and a dB readout. The strip owns no session state — it reports raw
// edits up to MixerPage, which applies them to the Session and engine.
class ChannelStrip : public QWidget {
  Q_OBJECT
public:
  explicit ChannelStrip(const QString& id, QWidget* parent = nullptr);

  const QString& id() const { return id_; }

  void setName(const QString& name);
  void setColor(const QColor& color);
  void setPan(int pan); // -100..+100
  void setGainDb(float db);
  void setMute(bool on);
  void setSolo(bool on);
  void setSelected(bool on);
  void setLevels(float peakL, float rmsL, float peakR, float rmsR,
                 bool clipped);

signals:
  void activated(const QString& id);
  void panChanged(const QString& id, int pan);
  void gainChanged(const QString& id, float db);
  void muteToggled(const QString& id, bool on);
  void soloToggled(const QString& id, bool on);

protected:
  void mousePressEvent(QMouseEvent* event) override;

private:
  QString id_;
  QString name_;
  QColor color_;
  Knob* knob_ = nullptr;
  Fader* fader_ = nullptr;
  VerticalMeter* meter_ = nullptr;
  QLabel* nameLabel_ = nullptr;
  QLabel* readout_ = nullptr;
  QPushButton* muteBtn_ = nullptr;
  QPushButton* soloBtn_ = nullptr;
  bool selected_ = false;
};

} // namespace misuc::ui