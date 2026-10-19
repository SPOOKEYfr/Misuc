#include "ChannelStrip.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "../Theme.h"
#include "../HintSystem.h"
#include "../branding/Icons.h"
#include "Fader.h"
#include "Knob.h"
#include "VerticalMeter.h"

namespace misuc::ui {

ChannelStrip::ChannelStrip(const QString& id, QWidget* parent)
    : QWidget(parent), id_(id) {
  setObjectName(QStringLiteral("channelStrip"));
  setMinimumWidth(84);
  setMaximumWidth(96);
  setCursor(Qt::PointingHandCursor);

  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(6, 10, 6, 8);
  lay->setSpacing(6);

  // Name + color dot.
  auto* nameRow = new QHBoxLayout();
  nameRow->setSpacing(4);
  auto* dot = new QLabel(this);
  dot->setFixedSize(8, 8);
  color_ = theme::kAccentSoft;
  dot->setStyleSheet(QStringLiteral("background: %1; border-radius: 4px;")
                         .arg(color_.name()));
  nameLabel_ = new QLabel(QStringLiteral("Track"), this);
  nameLabel_->setFont(theme::uiFont(9, QFont::DemiBold));
  nameLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  nameRow->addWidget(dot);
  nameRow->addWidget(nameLabel_, 1);
  lay->addLayout(nameRow);

  // Pan knob.
  knob_ = new Knob(this);
  knob_->setRange(-100, 100);
  knob_->setCenter(0);
  knob_->setTitle(QStringLiteral("PAN"));
  lay->addWidget(knob_, 0, Qt::AlignHCenter);

  // Mute / solo / record-arm.
  auto* ms = new QHBoxLayout();
  ms->setSpacing(4);
  muteBtn_ = new QPushButton(QStringLiteral("M"), this);
  soloBtn_ = new QPushButton(QStringLiteral("S"), this);
  armBtn_ = new QPushButton(QStringLiteral("R"), this);
  for (QPushButton* b : {muteBtn_, soloBtn_, armBtn_}) {
    b->setCheckable(true);
    b->setFixedSize(20, 24);
    b->setCursor(Qt::PointingHandCursor);
    b->setFont(theme::monoFont(8));
  }
  armBtn_->setObjectName(QStringLiteral("arm_") + id_);
  armBtn_->setToolTip(QStringLiteral("Arm this track for input recording; the "
                                     "recorded clip lands here"));
  ms->addWidget(muteBtn_);
  ms->addWidget(soloBtn_);
  ms->addWidget(armBtn_);
  lay->addLayout(ms);

  // Routing selector: MASTER plus one entry per bus.
  routeCombo_ = new QComboBox(this);
  routeCombo_->setFixedHeight(20);
  routeCombo_->setCursor(Qt::PointingHandCursor);
  routeCombo_->setToolTip(QStringLiteral("Route: master output or a bus"));
  connect(routeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int index) { emit routeChanged(id_, index - 1); });
  lay->addWidget(routeCombo_);

  // Auxiliary controls: parallel sends (every strip) and bus FX (only buses).
  auto* auxRow = new QHBoxLayout();
  auxRow->setSpacing(4);
  sendsBtn_ = new QPushButton(QStringLiteral("↗"), this);
  sendsBtn_->setFixedSize(20, 18);
  sendsBtn_->setCursor(Qt::PointingHandCursor);
  sendsBtn_->setFont(theme::monoFont(8));
  sendsBtn_->setToolTip(QStringLiteral("Sends: parallel aux/duck taps to buses"));
  fxBtn_ = new QPushButton(QStringLiteral("FX"), this);
  fxBtn_->setFixedSize(24, 18);
  fxBtn_->setCursor(Qt::PointingHandCursor);
  fxBtn_->setFont(theme::monoFont(8));
  fxBtn_->setToolTip(QStringLiteral("Bus FX: insert chain on this subgroup"));
  fxBtn_->hide();
  connect(sendsBtn_, &QPushButton::clicked, this,
          [this] { emit sendsRequested(id_); });
  connect(fxBtn_, &QPushButton::clicked, this,
          [this] { emit fxRequested(id_); });
  auxRow->addStretch();
  auxRow->addWidget(sendsBtn_);
  auxRow->addWidget(fxBtn_);
  auxRow->addStretch();
  lay->addLayout(auxRow);

  // Meter.
  meter_ = new VerticalMeter(this);
  lay->addWidget(meter_, 1);

  // Fader.
  fader_ = new Fader(this);
  lay->addWidget(fader_, 0, Qt::AlignHCenter);

  // Gain readout.
  readout_ = new QLabel(QStringLiteral("0.0"), this);
  readout_->setAlignment(Qt::AlignCenter);
  readout_->setFont(theme::monoFont(8));
  lay->addWidget(readout_);

  connect(knob_, &Knob::valueChanged, this, [this](int v) {
    emit panChanged(id_, v);
  });
  connect(fader_, &Fader::valueChanged, this, [this](float db) {
    readout_->setText(QString::number(db, 'f', 1));
    emit gainChanged(id_, db);
  });
  connect(muteBtn_, &QPushButton::toggled, this,
          [this](bool on) { emit muteToggled(id_, on); });
  connect(soloBtn_, &QPushButton::toggled, this,
          [this](bool on) { emit soloToggled(id_, on); });
  connect(armBtn_, &QPushButton::toggled, this,
          [this](bool on) { emit armToggled(id_, on); });

  setSelected(false);
}

void ChannelStrip::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton)
    emit activated(id_);
  QWidget::mousePressEvent(event);
}

void ChannelStrip::setName(const QString& name) {
  name_ = name;
  nameLabel_->setText(nameLabel_->fontMetrics().elidedText(
      name, Qt::ElideRight, width() - 24));
}

void ChannelStrip::setColor(const QColor& color) {
  color_ = color;
  auto* dot = qobject_cast<QLabel*>(layout()->itemAt(0)->layout()->itemAt(0)->widget());
  if (dot)
    dot->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 4px;").arg(color.name()));
}

void ChannelStrip::setPan(int pan) {
  knob_->setValue(std::clamp(pan, -100, 100));
}

void ChannelStrip::setGainDb(float db) {
  fader_->setValueDb(db);
  readout_->setText(QString::number(fader_->valueDb(), 'f', 1));
}

void ChannelStrip::setMute(bool on) {
  muteBtn_->setChecked(on);
}

void ChannelStrip::setSolo(bool on) {
  soloBtn_->setChecked(on);
}

void ChannelStrip::setArmed(bool on) {
  armBtn_->setChecked(on);
}

void ChannelStrip::setRoutes(const QStringList& busNames, int busIndex) {
  if (!routeCombo_)
    return;
  const QSignalBlocker blocker(routeCombo_);
  routeCombo_->clear();
  routeCombo_->addItem(QStringLiteral("MASTER"));
  routeCombo_->addItems(busNames);
  routeCombo_->setCurrentIndex(std::clamp(busIndex + 1, 0,
                                          routeCombo_->count() - 1));
}

void ChannelStrip::hideRoutes() {
  if (routeCombo_)
    routeCombo_->hide();
}

void ChannelStrip::showSendsButton(bool show) {
  if (sendsBtn_)
    sendsBtn_->setVisible(show);
}

void ChannelStrip::showFxButton(bool show) {
  if (fxBtn_)
    fxBtn_->setVisible(show);
}

void ChannelStrip::setSelected(bool on) {
  selected_ = on;
  if (on)
    setStyleSheet(QStringLiteral(
        "QWidget { background-color: %1; border: 1px solid %2; "
        "border-radius: 10px; }")
                      .arg(theme::kGlass.name(QColor::HexArgb),
                           theme::kAccent.name()));
  else
    setStyleSheet(QStringLiteral(
        "QWidget { background-color: %1; border: 1px solid %2; "
        "border-radius: 10px; }")
                      .arg(theme::kGlass.name(QColor::HexArgb),
                           theme::kGlassBorder.name(QColor::HexArgb)));
}

void ChannelStrip::setLevels(float peakL, float rmsL, float peakR, float rmsR,
                             bool clipped) {
  meter_->setLevels(peakL, rmsL, peakR, rmsR, clipped);
}

namespace {
// Shared dB display helper: -inf for fully closed faders.
inline QString dbText(float db) {
  return db <= -90.f ? QStringLiteral("-inf")
                     : QString::number(db, 'f', 1) + QStringLiteral(" dB");
}
inline QString panText(int pan) {
  if (pan < -1) return QStringLiteral("L%1").arg(-pan);
  if (pan > 1) return QStringLiteral("R%1").arg(pan);
  return QStringLiteral("C");
}
} // namespace

void ChannelStrip::attachHints(HintManager* hints) {
  if (!hints)
    return;
  // Every live-value provider is guarded with a QPointer so the HintManager
  // never dereferences a strip that is being rebuilt/destroyed.
  const QPointer<ChannelStrip> self(this);
  const QString n = name_;
  hints->registerTool(fader_, QStringLiteral("%1 · Volume").arg(n),
                      QStringLiteral("Per-track fader gain, applied live by the "
                                     "audio engine to this channel"),
                      QString(), icons::IconId::None,
                      [self]() -> QString {
                        if (self.isNull()) return QString();
                        return dbText(self->fader_->valueDb());
                      });
  hints->registerTool(knob_, QStringLiteral("%1 · Pan").arg(n),
                      QStringLiteral("Stereo pan (constant-power, centre-detent)"),
                      QString(), icons::IconId::None,
                      [self]() -> QString {
                        if (self.isNull()) return QString();
                        return panText(self->knob_->value());
                      });
  hints->registerTool(muteBtn_, QStringLiteral("%1 · Mute").arg(n),
                      QStringLiteral("Silence this channel in the mix"),
                      QString(), icons::IconId::None,
                      [self]() -> QString {
                        if (self.isNull()) return QString();
                        return self->muteBtn_->isChecked() ? QStringLiteral("muted")
                                                           : QStringLiteral("off");
                      });
  hints->registerTool(soloBtn_, QStringLiteral("%1 · Solo").arg(n),
                      QStringLiteral("Isolate this channel; others fall into "
                                     "any-solo logic"),
                      QString(), icons::IconId::None,
                      [self]() -> QString {
                        if (self.isNull()) return QString();
                        return self->soloBtn_->isChecked() ? QStringLiteral("solo")
                                                           : QStringLiteral("off");
                      });
  hints->registerTool(armBtn_, QStringLiteral("%1 · Record arm").arg(n),
                      QStringLiteral("Arm this track for input recording; the "
                                     "recorded clip lands on it"),
                      QString(), icons::IconId::None,
                      [self]() -> QString {
                        if (self.isNull()) return QString();
                        return self->armBtn_->isChecked()
                                   ? QStringLiteral("armed")
                                   : QStringLiteral("off");
                      });
  hints->registerTool(meter_, QStringLiteral("%1 · Level").arg(n),
                      QStringLiteral("Post-fader signal, peak + RMS both sides"),
                      QString(), icons::IconId::None,
                      [self]() -> QString {
                        if (self.isNull()) return QString();
                        auto* m = self->meter_;
                        return QStringLiteral("L %1 / R %2%3")
                            .arg(dbText(20.f * std::log10(std::max(1e-6f, m->peakL()))),
                                 dbText(20.f * std::log10(std::max(1e-6f, m->peakR()))),
                                 m->clipped() ? QStringLiteral(" CLIP") : QString());
                      });
  hints->registerTool(nameLabel_, QStringLiteral("%1").arg(n),
                      QStringLiteral("Click to select this mixer channel"),
                      QString(), icons::IconId::None, nullptr);
  if (routeCombo_)
    hints->registerTool(routeCombo_, QStringLiteral("%1 · Route").arg(n),
                        QStringLiteral("Send the channel to the master output "
                                       "or a named bus"), QString(),
                        icons::IconId::None,
                        [self]() -> QString {
                          if (self.isNull() || !self->routeCombo_) return QString();
                          return self->routeCombo_->currentText();
                        });
}

} // namespace misuc::ui