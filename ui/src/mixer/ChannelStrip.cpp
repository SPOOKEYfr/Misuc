#include "ChannelStrip.h"

#include <QHBoxLayout>
#include <QMouseEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "../Theme.h"
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
  color_ = QColor(0xff, 0x6b, 0x4a);
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

  // Mute / solo.
  auto* ms = new QHBoxLayout();
  ms->setSpacing(4);
  muteBtn_ = new QPushButton(QStringLiteral("M"), this);
  soloBtn_ = new QPushButton(QStringLiteral("S"), this);
  for (QPushButton* b : {muteBtn_, soloBtn_}) {
    b->setCheckable(true);
    b->setFixedSize(26, 24);
    b->setCursor(Qt::PointingHandCursor);
    b->setFont(theme::monoFont(8));
  }
  ms->addWidget(muteBtn_);
  ms->addWidget(soloBtn_);
  lay->addLayout(ms);

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

} // namespace misuc::ui