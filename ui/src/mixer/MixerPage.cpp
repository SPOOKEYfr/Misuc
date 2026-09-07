#include "MixerPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>

#include <misuc/engine/project/Session.h>

#include "../Theme.h"
#include "ChannelStrip.h"
#include "Fader.h"
#include "Knob.h"
#include "VerticalMeter.h"

using misuc::project::Session;
using misuc::project::Track;

namespace misuc::ui {

namespace {
// Per-track accent colors (rotated by track index). The first channel follows
// the active theme accent instead of a fixed literal so the mixer never
// hardcodes brand colors; the rest stay on the stable rotation.
const QColor kStripColors[] = {
    QColor(0xff, 0x6b, 0x4a), QColor(0x22, 0xd3, 0xee),
    QColor(0x4a, 0xde, 0x80), QColor(0xa9, 0x70, 0xff),
    QColor(0xff, 0xb4, 0x54), QColor(0xff, 0x54, 0x70),
    QColor(0x4f, 0x8b, 0xff), QColor(0x34, 0xd3, 0x99),
};
} // namespace

MixerPage::MixerPage(Session& session, misuc::engine::AudioEngine& engine,
                     QWidget* parent)
    : QWidget(parent), session_(&session), engine_(engine) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 24);
  root->setSpacing(16);

  {
    auto* t = new QLabel(QStringLiteral("Mixer"), this);
    t->setFont(theme::uiFont(13, QFont::Bold));
    root->addWidget(t);
    auto* s = new QLabel(
        QStringLiteral("Gain, pan, mute and solo are applied live by the audio "
                       "engine; meters read real signal. Export honors this "
                       "mix. Drag the fader, wheel fine-tunes, double-click "
                       "resets to 0 dB / centre pan."),
        this);
    s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    s->setWordWrap(true);
    root->addWidget(s);
  }

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  stripsHost_ = new QWidget(scroll_);
  stripsHost_->setObjectName(QStringLiteral("mixerHost"));
  scroll_->setWidget(stripsHost_);
  root->addWidget(scroll_, 1);

  rebuildStrips();

  // Apply the persisted master gain to the live fader + engine (no signal: the
  // fader only reports user interaction), so a loaded project's master bus is
  // heard exactly as saved.
  masterFader_->setValueDb(session.masterGainDb());
  masterReadout_->setText(QString::number(session.masterGainDb(), 'f', 1));
  engine_.setMasterGainDb(session.masterGainDb());
}

QColor MixerPage::stripColor(size_t index) const {
  constexpr size_t kCount =
      sizeof(kStripColors) / sizeof(kStripColors[0]);
  if (index == 0)
    return theme::kAccent;
  return kStripColors[index % kCount];
}

void MixerPage::refresh() { rebuildStrips(); }

int MixerPage::trackIndex(const QString& id) const {
  int i = 0;
  for (const Track& t : session_->tracks()) {
    if (QString::fromStdString(t.id) == id)
      return i;
    ++i;
  }
  return -1;
}

void MixerPage::rebuildStrips() {
  const QString keep = selectedId_;
  syncing_ = true;
  for (ChannelStrip* s : strips_)
    s->deleteLater();
  strips_.clear();
  if (auto* old = stripsHost_->layout()) {
    while (QLayoutItem* item = old->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    delete old;
  }

  auto* lay = new QHBoxLayout(stripsHost_);
  lay->setContentsMargins(4, 4, 4, 4);
  lay->setSpacing(12);
  lay->addStretch();

  int i = 0;
  for (const Track& t : session_->tracks()) {
    auto* strip = new ChannelStrip(QString::fromStdString(t.id), stripsHost_);
    strip->setName(QString::fromStdString(t.name));
    strip->setColor(stripColor(i));
    strip->setPan(static_cast<int>(std::lround(t.pan * 100.f)));
    strip->setGainDb(t.gainDb);
    strip->setMute(t.muted);
    strip->setSolo(t.solo);
    connect(strip, &ChannelStrip::activated, this,
            &MixerPage::onStripActivated);
    connect(strip, &ChannelStrip::panChanged, this, &MixerPage::onPanChanged);
    connect(strip, &ChannelStrip::gainChanged, this,
            &MixerPage::onGainChanged);
    connect(strip, &ChannelStrip::muteToggled, this, &MixerPage::onMuteToggled);
    connect(strip, &ChannelStrip::soloToggled, this, &MixerPage::onSoloToggled);
    lay->addWidget(strip);
    strips_.push_back(strip);
    ++i;
  }

  // Master bus strip (no pan/mute/solo — a true stereo master).
  auto* master = new QWidget(stripsHost_);
  master->setObjectName(QStringLiteral("masterStrip"));
  master->setMinimumWidth(84);
  master->setMaximumWidth(96);
  auto* ml = new QVBoxLayout(master);
  ml->setContentsMargins(6, 10, 6, 8);
  ml->setSpacing(6);
  auto* mName = new QLabel(QStringLiteral("MASTER"), master);
  mName->setAlignment(Qt::AlignCenter);
  mName->setFont(theme::uiFont(9, QFont::DemiBold));
  ml->addWidget(mName);
  masterMeter_ = new VerticalMeter(master);
  ml->addWidget(masterMeter_, 1);
  masterFader_ = new Fader(master);
  ml->addWidget(masterFader_, 0, Qt::AlignHCenter);
  masterReadout_ = new QLabel(QStringLiteral("0.0"), master);
  masterReadout_->setAlignment(Qt::AlignCenter);
  masterReadout_->setFont(theme::monoFont(8));
  ml->addWidget(masterReadout_);
  connect(masterFader_, &Fader::valueChanged, this,
          &MixerPage::onMasterGainChanged);
  lay->addWidget(master);

  lay->addStretch();

  selectedId_ = keep;
  syncing_ = false;

  // Restore selection after rebuild.
  bool selected = false;
  for (ChannelStrip* s : strips_)
    if (s->id() == selectedId_) {
      s->setSelected(true);
      selected = true;
      break;
    }
  if (!selected && !strips_.empty()) {
    selectedId_ = strips_.front()->id();
    strips_.front()->setSelected(true);
  }
}

void MixerPage::syncFromSession() {
  if (strips_.size() != session_->tracks().size()) {
    rebuildStrips();
    return;
  }
  bool mismatch = false;
  for (size_t i = 0; i < strips_.size(); ++i)
    if (strips_[i]->id() != QString::fromStdString(session_->tracks()[i].id))
      mismatch = true;
  if (mismatch) {
    rebuildStrips();
    return;
  }
  syncing_ = true;
  size_t i = 0;
  for (const Track& t : session_->tracks()) {
    strips_[i]->setName(QString::fromStdString(t.name));
    strips_[i]->setColor(stripColor(i));
    strips_[i]->setPan(static_cast<int>(std::lround(t.pan * 100.f)));
    strips_[i]->setGainDb(t.gainDb);
    strips_[i]->setMute(t.muted);
    strips_[i]->setSolo(t.solo);
    ++i;
  }
  if (masterFader_) {
    masterFader_->setValueDb(session_->masterGainDb());
    masterReadout_->setText(QString::number(session_->masterGainDb(), 'f', 1));
  }
  syncing_ = false;
}

void MixerPage::syncToEngine() {
  engine_.setMasterGainDb(session_->masterGainDb());
  std::vector<misuc::engine::MixerTrackParam> params;
  for (const Track& t : session_->tracks()) {
    params.push_back(misuc::engine::MixerTrackParam{
        t.gainDb, std::clamp(t.pan, -1.f, 1.f), t.muted, t.solo});
  }
  engine_.publishMixer(std::move(params));
}

void MixerPage::pollMeters() {
  size_t i = 0;
  for (ChannelStrip* s : strips_) {
    s->setLevels(engine_.trackMeterPeakL(static_cast<uint32_t>(i)),
                 engine_.trackMeterRmsL(static_cast<uint32_t>(i)),
                 engine_.trackMeterPeakR(static_cast<uint32_t>(i)),
                 engine_.trackMeterRmsR(static_cast<uint32_t>(i)),
                 engine_.trackMeterClipped(static_cast<uint32_t>(i)));
    ++i;
  }
  if (masterMeter_) {
    masterMeter_->setLevels(engine_.meterPeakL(), engine_.meterRmsL(),
                            engine_.meterPeakR(), engine_.meterRmsR(),
                            engine_.meterClipped());
  }
}

void MixerPage::selectTrack(const QString& id) {
  if (syncing_)
    return;
  bool found = false;
  for (ChannelStrip* s : strips_) {
    const bool sel = s->id() == id;
    s->setSelected(sel);
    if (sel)
      found = true;
  }
  if (!found && !strips_.empty()) {
    strips_.front()->setSelected(true);
    selectedId_ = strips_.front()->id();
  } else if (found) {
    selectedId_ = id;
  }
}

void MixerPage::onStripActivated(const QString& id) {
  selectedId_ = id;
  for (ChannelStrip* s : strips_)
    s->setSelected(s->id() == id);
  emit trackSelected(id);
}

void MixerPage::onPanChanged(const QString& id, int pan) {
  if (syncing_)
    return;
  Track* t = session_->findTrack(id.toStdString());
  if (!t)
    return;
  t->pan = std::clamp(static_cast<float>(pan) / 100.f, -1.f, 1.f);
  emit edited();
}

void MixerPage::onGainChanged(const QString& id, float db) {
  if (syncing_)
    return;
  Track* t = session_->findTrack(id.toStdString());
  if (!t)
    return;
  t->gainDb = std::clamp(db, -60.f, 12.f);
  emit edited();
}

void MixerPage::onMuteToggled(const QString& id, bool on) {
  if (syncing_)
    return;
  Track* t = session_->findTrack(id.toStdString());
  if (!t)
    return;
  t->muted = on;
  emit edited();
}

void MixerPage::onSoloToggled(const QString& id, bool on) {
  if (syncing_)
    return;
  Track* t = session_->findTrack(id.toStdString());
  if (!t)
    return;
  t->solo = on;
  emit edited();
}

void MixerPage::onMasterGainChanged(float db) {
  masterReadout_->setText(QString::number(db, 'f', 1));
  engine_.setMasterGainDb(db);
  session_->setMasterGainDb(db);
  emit edited();
  syncToEngine(); // keep published mixer params in sync with master
}

} // namespace misuc::ui