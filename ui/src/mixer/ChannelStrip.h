#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

namespace misuc::ui {

class Fader;
class Knob;
class VerticalMeter;
class HintManager;

// One mixer channel: name + color dot, routing selector, pan knob, mute/solo,
// stereo meter, fader, and a dB readout. The strip owns no session state — it
// reports raw edits up to MixerPage, which applies them to the Session and
// engine. The route combo lists the master output plus every bus; index 0 is
// the master and the reported busIndex is the combo index minus one (-1 =
// master), matching the engine's bus vector.
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
  void setArmed(bool on); // input-recording armed (record-target channel)
  void setLevels(float peakL, float rmsL, float peakR, float rmsR,
                 bool clipped);
  // Populates the route combo (index 0 = MASTER, then one entry per bus) and
  // selects the given destination without emitting. `busIndex` -1 = master.
  void setRoutes(const QStringList& busNames, int busIndex);
  // Buses themselves always feed the master: hide the redundant route combo.
  void hideRoutes();
  // Reveal/hide the auxiliary "Sends" and "FX" buttons. Track strips show the
  // send (aux/duck bus) button; bus strips additionally show the FX button
  // (their own post-fader chain, e.g. a sidechain compressor).
  void showSendsButton(bool show);
  void showFxButton(bool show);

  // Registers exact-control hints (volume/pan/mute/solo/meter) on the strip's
  // own widgets so the HintBar mirrors the real mixer value under the cursor.
  void attachHints(HintManager* hints);

signals:
  void activated(const QString& id);
  void panChanged(const QString& id, int pan);
  void gainChanged(const QString& id, float db);
  void muteToggled(const QString& id, bool on);
  void soloToggled(const QString& id, bool on);
  void armToggled(const QString& id, bool on);
  void routeChanged(const QString& id, int busIndex);
  void sendsRequested(const QString& id);
  void fxRequested(const QString& id);

protected:
  void mousePressEvent(QMouseEvent* event) override;

private:
  QString id_;
  QString name_;
  QColor color_;
  QComboBox* routeCombo_ = nullptr;
  QPushButton* sendsBtn_ = nullptr;
  QPushButton* fxBtn_ = nullptr;
  Knob* knob_ = nullptr;
  Fader* fader_ = nullptr;
  VerticalMeter* meter_ = nullptr;
  QLabel* nameLabel_ = nullptr;
  QLabel* readout_ = nullptr;
  QPushButton* muteBtn_ = nullptr;
  QPushButton* soloBtn_ = nullptr;
  QPushButton* armBtn_ = nullptr;
  bool selected_ = false;
};

} // namespace misuc::ui