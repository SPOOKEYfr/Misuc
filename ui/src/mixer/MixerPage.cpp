#include "MixerPage.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>

#include <misuc/engine/dsp/Dsp.h>
#include <misuc/engine/project/Session.h>

#include "../Theme.h"
#include "../HintSystem.h"
#include "../branding/Icons.h"
#include "ChannelStrip.h"
#include "Fader.h"
#include "Knob.h"
#include "RoutingDialogs.h"
#include "VerticalMeter.h"

using misuc::project::Bus;
using misuc::project::Send;
using misuc::project::Session;
using misuc::project::Track;

namespace misuc::ui {

namespace {
// Per-track accent colors (rotated by track index). The first channel follows
// the active theme accent instead of a fixed literal so the mixer never
// hardcodes brand colors; the rest stay on a stable rotation tuned to the
// design system — a monochrome violet/slate ladder on near-black, so the
// mixer reads black-first with violet second (no rainbow/gaming look).
const QColor kStripColors[] = {
    QColor(0x8f, 0x7b, 0xfa), // violet accent
    QColor(0x9a, 0x8f, 0xc8), // muted lavender
    QColor(0x6a, 0x63, 0x82), // dusky violet
    QColor(0x51, 0x54, 0x63), // cool slate
    QColor(0x5e, 0x57, 0x68), // mauve-gray
    QColor(0x48, 0x4d, 0x58), // neutral graphite
    QColor(0x63, 0x5f, 0x73), // plum-slate
    QColor(0x57, 0x58, 0x62), // stone gray
};
// Bus strips use the same ladder with a rotation offset so groups read as a
// distinct band from the raw tracks.
constexpr size_t kBusHueOffset = 4;
} // namespace

MixerPage::MixerPage(Session& session, misuc::engine::AudioEngine& engine,
                     QWidget* parent)
    : QWidget(parent), session_(&session), engine_(engine) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(16, 10, 16, 10);
  root->setSpacing(10);

  {
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    auto* t = new QLabel(QStringLiteral("Mixer"), this);
    t->setFont(theme::uiFont(13, QFont::Bold));
    head->addWidget(t);
    auto* addBus = new QPushButton(QStringLiteral("+ Bus"), this);
    addBus->setObjectName(QStringLiteral("addBusBtn"));
    addBus->setIcon(icons::make(icons::IconId::Add, theme::kText, 14));
    addBus->setIconSize(QSize(14, 14));
    addBus->setCursor(Qt::PointingHandCursor);
    addBus->setToolTip(QStringLiteral(
        "Create a bus: a named subgroup that routed tracks feed into"));
    connect(addBus, &QPushButton::clicked, this, &MixerPage::onAddBus);
    head->addWidget(addBus);
    head->addStretch();
    root->addLayout(head);
    auto* s = new QLabel(
        QStringLiteral("Gain, pan, mute and solo are applied live by the audio "
                       "engine; meters read real signal. Export honors this "
                       "mix. Route a track into a bus to control a whole "
                       "subgroup with one fader. Drag the fader, wheel "
                       "fine-tunes, double-click resets to 0 dB / centre pan."),
        this);
    s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    s->setWordWrap(true);
    root->addWidget(s);
  }

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scroll_->setMinimumHeight(110);
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

QColor MixerPage::busStripColor(size_t index) const {
  constexpr size_t kCount =
      sizeof(kStripColors) / sizeof(kStripColors[0]);
  return kStripColors[(index + kBusHueOffset) % kCount];
}

void MixerPage::refresh() { rebuildStrips(); }

void MixerPage::attachHints(HintManager* hints) {
  hints_ = hints;
  if (!hints)
    return;
  if (auto* addBus = findChild<QPushButton*>(QStringLiteral("addBusBtn")))
    hints->registerTool(addBus, QStringLiteral("Add Bus"),
                        QStringLiteral("Create a named bus for track subgroups"),
                        QString(), icons::IconId::Add);
  for (ChannelStrip* s : strips_)
    s->attachHints(hints);
  if (masterFader_) {
    const QPointer<MixerPage> self(this);
    auto dbText = [](float db) {
      return db <= -90.f ? QStringLiteral("-inf")
                         : QString::number(db, 'f', 1) + QStringLiteral(" dB");
    };
    hints->registerTool(masterFader_, QStringLiteral("Master · Volume"),
                        QStringLiteral("Master bus gain applied to the whole mix"),
                        QString(), icons::IconId::None,
                        [self, dbText]() -> QString {
                          if (self.isNull() || !self->masterFader_)
                            return QString();
                          return dbText(self->masterFader_->valueDb());
                        });
    if (masterMeter_) {
      hints->registerTool(masterMeter_, QStringLiteral("Master · Level"),
                          QStringLiteral("Master bus output, peak + RMS stereo"),
                          QString(), icons::IconId::None,
                          [self]() -> QString {
                            if (self.isNull() || !self->masterMeter_)
                              return QString();
                            auto* m = self->masterMeter_;
                            auto db = [](float x) {
                              return x > 1e-6f ? 20.f * std::log10(x) : -120.f;
                            };
                            return QStringLiteral("L %1 / R %2%3")
                                .arg(db(static_cast<float>(m->peakL())), 0, 'f', 1)
                                .arg(db(static_cast<float>(m->peakR())), 0, 'f', 1)
                                .arg(m->clipped() ? QStringLiteral(" CLIP")
                                                  : QString());
                          });
    }
  }
}

int MixerPage::trackIndex(const QString& id) const {
  int i = 0;
  for (const Track& t : session_->tracks()) {
    if (QString::fromStdString(t.id) == id)
      return i;
    ++i;
  }
  return -1;
}

// Bus order in the session == the published bus vector order == the engine's
// busIndex space the track strips report.
int MixerPage::busIndex(const QString& busId) const {
  int i = 0;
  for (const Bus& b : session_->buses()) {
    if (QString::fromStdString(b.id) == busId)
      return i;
    ++i;
  }
  return -1;
}

// The current destination (as an engine busIndex) for one track.
static int busIndexFor(const Session& s, const Track& t) {
  int i = 0;
  for (const Bus& b : s.buses()) {
    if (b.id == t.busId)
      return i;
    ++i;
  }
  return -1;
}

void MixerPage::rebuildStrips() {
  const QString keep = selectedId_;
  syncing_ = true;
  strips_.clear();
  busStrips_.clear();
  busDeleteBtns_.clear();
  // Tear the whole old mixer tree down asynchronously. A rebuild can be
  // triggered from inside one of a strip's own signals (fader/knob/button),
  // so the widgets must never be freed synchronously while that signal is
  // still being delivered - schedule every child for deleteLater and only
  // destroy the (now emptied) layout items here.
  if (auto* old = stripsHost_->layout()) {
    while (QLayoutItem* item = old->takeAt(0)) {
      if (QWidget* w = item->widget()) {
        w->hide();
        w->setParent(nullptr);
        w->deleteLater();
      }
      delete item;
    }
    delete old;
  }
  for (QWidget* w : stripsHost_->findChildren<QWidget*>())
    w->deleteLater();

  auto* lay = new QHBoxLayout(stripsHost_);
  lay->setContentsMargins(4, 4, 4, 4);
  lay->setSpacing(12);
  lay->addStretch();

  QStringList busNames;
  for (const Bus& b : session_->buses())
    busNames << QString::fromStdString(b.name);

  int i = 0;
  for (const Track& t : session_->tracks()) {
    auto* strip = new ChannelStrip(QString::fromStdString(t.id), stripsHost_);
    strip->setName(QString::fromStdString(t.name));
    strip->setColor(stripColor(i));
    strip->setPan(static_cast<int>(std::lround(t.pan * 100.f)));
    strip->setGainDb(t.gainDb);
    strip->setMute(t.muted);
    strip->setSolo(t.solo);
    strip->setArmed(t.recordArm);
    strip->setRoutes(busNames, busIndexFor(*session_, t));
    connect(strip, &ChannelStrip::activated, this,
            &MixerPage::onStripActivated);
    connect(strip, &ChannelStrip::panChanged, this, &MixerPage::onPanChanged);
    connect(strip, &ChannelStrip::gainChanged, this,
            &MixerPage::onGainChanged);
    connect(strip, &ChannelStrip::muteToggled, this, &MixerPage::onMuteToggled);
    connect(strip, &ChannelStrip::soloToggled, this, &MixerPage::onSoloToggled);
    connect(strip, &ChannelStrip::armToggled, this, &MixerPage::onArmToggled);
    connect(strip, &ChannelStrip::routeChanged, this,
            &MixerPage::onRouteChanged);
    connect(strip, &ChannelStrip::sendsRequested, this,
            &MixerPage::onSendsRequested);
    connect(strip, &ChannelStrip::fxRequested, this, &MixerPage::onFxRequested);
    strip->attachHints(hints_);
    lay->addWidget(strip);
    strips_.push_back(strip);
    ++i;
  }

  // Named bus strips: a whole subgroup of routed tracks behind one fader.
  for (size_t bi = 0; bi < session_->buses().size(); ++bi) {
    const Bus& b = session_->buses()[bi];
    const QString busId = QString::fromStdString(b.id);
    auto* wrap = new QWidget(stripsHost_);
    auto* wl = new QVBoxLayout(wrap);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(2);
    auto* delRow = new QHBoxLayout();
    delRow->setSpacing(0);
    delRow->addStretch();
    auto* del = new QPushButton(QStringLiteral("×"), wrap);
    del->setObjectName(busId + QStringLiteral("_delBus"));
    del->setIcon(icons::make(icons::IconId::Close, theme::kTextDim, 12));
    del->setIconSize(QSize(12, 12));
    del->setFixedSize(18, 18);
    del->setCursor(Qt::PointingHandCursor);
    del->setToolTip(QStringLiteral("Delete this bus (routed tracks fall back "
                                   "to the master output)"));
    delRow->addWidget(del);
    delRow->addStretch();
    wl->addLayout(delRow);

    auto* strip = new ChannelStrip(QStringLiteral("bus:") + busId, wrap);
    strip->setObjectName(busId + QStringLiteral("_busStrip"));
    strip->setName(QString::fromStdString(b.name));
    strip->setColor(busStripColor(bi));
    strip->setPan(static_cast<int>(std::lround(b.pan * 100.f)));
    strip->setGainDb(b.faderDb);
    strip->setMute(b.muted);
    strip->setSolo(b.solo);
    // Bus routing combo: MASTER + every bus. Routing a bus back onto itself is
    // ignored by the Session (a cycle would be rejected anyway).
    strip->setRoutes(busNames, busIndex(QString::fromStdString(b.target)));
    strip->showFxButton(true);
    connect(strip, &ChannelStrip::routeChanged, this,
            &MixerPage::onRouteChanged);
    connect(strip, &ChannelStrip::sendsRequested, this,
            &MixerPage::onSendsRequested);
    connect(strip, &ChannelStrip::fxRequested, this, &MixerPage::onFxRequested);
    connect(strip, &ChannelStrip::panChanged, this,
            &MixerPage::onBusPanChanged);
    connect(strip, &ChannelStrip::gainChanged, this,
            &MixerPage::onBusGainChanged);
    connect(strip, &ChannelStrip::muteToggled, this,
            &MixerPage::onBusMuteToggled);
    connect(strip, &ChannelStrip::soloToggled, this,
            &MixerPage::onBusSoloToggled);
    strip->attachHints(hints_);
    wl->addWidget(strip);
    lay->addWidget(wrap);
    busStrips_.push_back(strip);
    busDeleteBtns_.push_back(del);
    if (hints_)
      hints_->registerTool(del, QStringLiteral("Delete %1 Bus").arg(QString::fromStdString(b.name)),
                           QStringLiteral("Remove this bus; routed tracks fall "
                                          "back to the master output"), QString(),
                           icons::IconId::Close);
    connect(del, &QPushButton::clicked, this,
            [this, busId] { onBusDeleted(busId); });
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
  if (strips_.size() != session_->tracks().size() ||
      busStrips_.size() != session_->buses().size()) {
    rebuildStrips();
    return;
  }
  bool mismatch = false;
  for (size_t i = 0; i < strips_.size(); ++i)
    if (strips_[i]->id() != QString::fromStdString(session_->tracks()[i].id))
      mismatch = true;
  for (size_t i = 0; i < busStrips_.size(); ++i)
    if (busStrips_[i]->id() !=
        QStringLiteral("bus:") + QString::fromStdString(session_->buses()[i].id))
      mismatch = true;
  if (mismatch) {
    rebuildStrips();
    return;
  }
  syncing_ = true;
  QStringList busNames;
  for (const Bus& b : session_->buses())
    busNames << QString::fromStdString(b.name);
  size_t i = 0;
  for (const Track& t : session_->tracks()) {
    strips_[i]->setName(QString::fromStdString(t.name));
    strips_[i]->setColor(stripColor(i));
    strips_[i]->setPan(static_cast<int>(std::lround(t.pan * 100.f)));
    strips_[i]->setGainDb(t.gainDb);
    strips_[i]->setMute(t.muted);
    strips_[i]->setSolo(t.solo);
    strips_[i]->setArmed(t.recordArm);
    strips_[i]->setRoutes(busNames, busIndexFor(*session_, t));
    ++i;
  }
  for (size_t b = 0; b < busStrips_.size(); ++b) {
    const Bus& bus = session_->buses()[b];
    busStrips_[b]->setName(QString::fromStdString(bus.name));
    busStrips_[b]->setColor(busStripColor(b));
    busStrips_[b]->setPan(static_cast<int>(std::lround(bus.pan * 100.f)));
    busStrips_[b]->setGainDb(bus.faderDb);
    busStrips_[b]->setMute(bus.muted);
    busStrips_[b]->setSolo(bus.solo);
    busStrips_[b]->setRoutes(busNames,
                             busIndex(QString::fromStdString(bus.target)));
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
        t.gainDb, std::clamp(t.pan, -1.f, 1.f), t.muted, t.solo,
        busIndexFor(*session_, t)});
  }
  std::vector<misuc::engine::MixerBusParam> buses;
  for (const Bus& b : session_->buses()) {
    buses.push_back(misuc::engine::MixerBusParam{
        b.faderDb, std::clamp(b.pan, -1.f, 1.f), b.muted, b.solo,
        busIndex(QString::fromStdString(b.target))});
  }
  // Parallel sends (Comp19): resolve session send ids to engine indices in one
  // snapshot so tracks, buses, routing and sends never diverge.
  std::vector<misuc::engine::MixerSendParam> sends;
  for (const Send& sd : session_->sends()) {
    if (!sd.enabled)
      continue;
    misuc::engine::MixerSendParam sp;
    const int tIdx = trackIndex(QString::fromStdString(sd.sourceTrack));
    const int bIdx = busIndex(QString::fromStdString(sd.sourceBus));
    const int tgt = busIndex(QString::fromStdString(sd.targetBus));
    if (tgt < 0)
      continue; // target bus gone: the renderer ignores it the same way
    if (!sd.sourceTrack.empty() && tIdx >= 0) {
      sp.sourceTrack = static_cast<uint8_t>(tIdx);
    } else if (!sd.sourceBus.empty() && bIdx >= 0) {
      sp.sourceBus = static_cast<uint8_t>(bIdx);
    } else {
      continue; // source vanished: nothing to tap
    }
    sp.target = static_cast<uint8_t>(tgt);
    sp.level = misuc::dsp::gainFromDb(std::clamp(sd.levelDb, -60.f, 12.f));
    sp.enabled = true;
    sends.push_back(sp);
  }
  engine_.publishMixer(std::move(params), std::move(buses), std::move(sends));
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
  for (size_t b = 0; b < busStrips_.size(); ++b) {
    busStrips_[b]->setLevels(engine_.busMeterPeakL(static_cast<uint32_t>(b)),
                             engine_.busMeterRmsL(static_cast<uint32_t>(b)),
                             engine_.busMeterPeakR(static_cast<uint32_t>(b)),
                             engine_.busMeterRmsR(static_cast<uint32_t>(b)),
                             engine_.busMeterClipped(static_cast<uint32_t>(b)));
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

void MixerPage::onArmToggled(const QString& id, bool on) {
  if (syncing_)
    return;
  Track* t = session_->findTrack(id.toStdString());
  if (!t)
    return;
  t->recordArm = on;
  emit edited();
}

void MixerPage::onMasterGainChanged(float db) {
  masterReadout_->setText(QString::number(db, 'f', 1));
  engine_.setMasterGainDb(db);
  session_->setMasterGainDb(db);
  emit edited();
  syncToEngine(); // keep published mixer params in sync with master
}

void MixerPage::onRouteChanged(const QString& id, int busIndex /* -1 = master */) {
  if (syncing_)
    return;
  // Bus strips carry a "bus:" prefix; their route selects the bus's target.
  if (id.startsWith(QStringLiteral("bus:"))) {
    onBusRouteChanged(id, busIndex);
    return;
  }
  Track* t = session_->findTrack(id.toStdString());
  if (!t)
    return;
  t->busId = busIndex >= 0 &&
                     busIndex < static_cast<int>(session_->buses().size())
                 ? session_->buses()[static_cast<size_t>(busIndex)].id
                 : std::string();
  emit edited();
}

void MixerPage::onBusRouteChanged(const QString& id, int busIndex) {
  const std::string busId = id.mid(4).toStdString();
  Bus* b = session_->findBus(busId);
  if (!b)
    return;
  // A bus routed onto itself is meaningless (and would trip the cycle guard);
  // keep the previous routing in that case.
  if (busIndex >= 0 && busIndex < static_cast<int>(session_->buses().size())) {
    const std::string target = session_->buses()[static_cast<size_t>(busIndex)].id;
    if (target == busId)
      return;
    if (session_->setBusTarget(busId, target))
      emit edited();
  } else {
    if (session_->setBusTarget(busId, std::string()))
      emit edited();
  }
}

void MixerPage::onSendsRequested(const QString& id) {
  if (syncing_)
    return;
  const bool isBus = id.startsWith(QStringLiteral("bus:"));
  const QString trackId = isBus ? QString() : id;
  const QString busId = isBus ? id.mid(4) : QString();
  SendDialog dlg(session_, trackId, busId, this);
  if (dlg.exec() == QDialog::Accepted) {
    rebuildStrips();
    emit edited();
    syncToEngine();
  }
}

void MixerPage::onFxRequested(const QString& id) {
  if (!id.startsWith(QStringLiteral("bus:")) || syncing_)
    return;
  BusFxDialog dlg(session_, id.mid(4), this);
  if (dlg.exec() == QDialog::Accepted) {
    emit edited(); // MainWindow applies the bus chains to the engine
  }
}

void MixerPage::onAddBus() {
  const size_t n = session_->buses().size();
  session_->addBus(QStringLiteral("Bus %1").arg(n + 1).toStdString());
  rebuildStrips();
  emit edited();
  syncToEngine();
}

void MixerPage::onBusDeleted(const QString& busId) {
  if (syncing_)
    return;
  if (!session_->removeBus(busId.toStdString()))
    return;
  rebuildStrips();
  emit edited();
  syncToEngine();
}

Bus* MixerPage::busById(const QString& id) {
  // The bus strips carry their session bus id after the "bus:" prefix.
  if (!id.startsWith(QStringLiteral("bus:")))
    return nullptr;
  return session_->findBus(id.mid(4).toStdString());
}

void MixerPage::onBusPanChanged(const QString& id, int pan) {
  if (syncing_)
    return;
  Bus* b = busById(id);
  if (!b)
    return;
  b->pan = std::clamp(static_cast<float>(pan) / 100.f, -1.f, 1.f);
  emit edited();
}

void MixerPage::onBusGainChanged(const QString& id, float db) {
  if (syncing_)
    return;
  Bus* b = busById(id);
  if (!b)
    return;
  b->faderDb = std::clamp(db, -60.f, 12.f);
  emit edited();
}

void MixerPage::onBusMuteToggled(const QString& id, bool on) {
  if (syncing_)
    return;
  Bus* b = busById(id);
  if (!b)
    return;
  b->muted = on;
  emit edited();
}

void MixerPage::onBusSoloToggled(const QString& id, bool on) {
  if (syncing_)
    return;
  Bus* b = busById(id);
  if (!b)
    return;
  b->solo = on;
  emit edited();
}

} // namespace misuc::ui