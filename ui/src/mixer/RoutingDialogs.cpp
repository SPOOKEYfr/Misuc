#include "RoutingDialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <misuc/engine/fx/BuiltinFx.h>
#include <misuc/engine/project/Session.h>

#include "../Theme.h"

namespace misuc::ui {

SendDialog::SendDialog(misuc::project::Session* session,
                       const QString& sourceTrackId,
                       const QString& sourceBusId, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("Sends"));
  setMinimumWidth(360);
  auto* lay = new QVBoxLayout(this);
  const bool fromBus = !sourceBusId.isEmpty();
  const std::string srcTrack = sourceTrackId.toStdString();
  const std::string srcBus = sourceBusId.toStdString();

  if (fromBus) {
    if (misuc::project::Bus* b = session->findBus(srcBus))
      lay->addWidget(new QLabel(
          QStringLiteral("Post-fader taps from bus \"%1\" to other buses.")
              .arg(QString::fromStdString(b->name)),
          this));
  } else if (misuc::project::Track* t = session->findTrack(srcTrack)) {
    lay->addWidget(new QLabel(
        QStringLiteral("Post-fader taps from track \"%1\" to buses. The dry "
                       "route always stays audible.")
            .arg(QString::fromStdString(t->name)),
        this));
  }

  // Current sends from this source, keyed by target bus id.
  std::map<std::string, const misuc::project::Send*> existing;
  for (const auto& sd : session->sends()) {
    const bool match =
        fromBus ? (sd.sourceBus == srcBus && sd.sourceTrack.empty())
                : (sd.sourceTrack == srcTrack && sd.sourceBus.empty());
    if (match)
      existing[sd.targetBus] = &sd;
  }

  struct Row {
    std::string busId;
    QCheckBox* checkbox = nullptr;
    QSlider* slider = nullptr;
  };
  std::vector<Row> rows;
  auto* grid = new QGridLayout();
  grid->setVerticalSpacing(6);
  int row = 0;
  for (const auto& b : session->buses()) {
    if (fromBus && b.id == srcBus)
      continue; // a bus cannot send to itself
    const auto it = existing.find(b.id);
    const bool on = it != existing.end() && it->second->enabled;
    const float db =
        it != existing.end() ? std::clamp(it->second->levelDb, -60.f, 12.f)
                             : -6.0f;
    auto* cb = new QCheckBox(QString::fromStdString(b.name), this);
    cb->setChecked(on);
    auto* lvl = new QSlider(Qt::Horizontal, this);
    lvl->setRange(-60, 12);
    lvl->setValue(static_cast<int>(std::lround(db)));
    auto* lbl = new QLabel(QString::number(db, 'f', 1) + QStringLiteral(" dB"),
                           this);
    lbl->setFont(theme::monoFont(8));
    connect(lvl, &QSlider::valueChanged, this,
            [lbl](int v) { lbl->setText(QString::number(float(v), 'f', 1) +
                                        QStringLiteral(" dB")); });
    connect(cb, &QCheckBox::toggled, this, [lvl](bool onn) {
      lvl->setEnabled(onn);
    });
    lvl->setEnabled(cb->isChecked());
    grid->addWidget(cb, row, 0);
    grid->addWidget(lvl, row, 1);
    grid->addWidget(lbl, row, 2);
    rows.push_back({b.id, cb, lvl});
    ++row;
  }
  lay->addLayout(grid);

  auto* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                           this);
  connect(buttons, &QDialogButtonBox::accepted, this,
          [this, session, fromBus, srcTrack, srcBus, rows]() {
            // Drop this source's sends wholesale, then rewrite from the rows.
            std::vector<std::string> doomed;
            for (const auto& sd : session->sends())
              if ((fromBus && sd.sourceBus == srcBus &&
                   sd.sourceTrack.empty()) ||
                  (!fromBus && sd.sourceTrack == srcTrack &&
                   sd.sourceBus.empty()))
                doomed.push_back(sd.id);
            for (const auto& id : doomed)
              session->removeSend(id);
            for (const auto& r : rows)
              if (r.checkbox->isChecked())
                session->addSend(fromBus ? std::string() : srcTrack,
                                 fromBus ? srcBus : std::string(),
                                 r.busId, float(r.slider->value()));
            accept();
          });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  lay->addWidget(buttons);
}

BusFxDialog::BusFxDialog(misuc::project::Session* session,
                         const QString& busIdIn, QWidget* parent)
    : QDialog(parent), session_(session), busId_(busIdIn.toStdString()) {
  setWindowTitle(QStringLiteral("Bus FX"));
  setMinimumWidth(380);
  auto* lay = new QVBoxLayout(this);
  misuc::project::Bus* bus = session_->findBus(busId_);
  if (!bus) {
    reject();
    return;
  }
  lay->addWidget(new QLabel(
      QStringLiteral("Post-fader insert chain on \"%1\". The sidechain "
                     "compressor keys off another bus's signal.")
          .arg(QString::fromStdString(bus->name)),
      this));

  rows_ = new QVBoxLayout();
  rows_->setSpacing(6);

  auto* addBtn = new QPushButton(QStringLiteral("+ SC Compressor"), this);
  addBtn->setCursor(Qt::PointingHandCursor);
  connect(addBtn, &QPushButton::clicked, this,
          [this] { session_->addBusFx(busId_, misuc::engine::kBuiltinCompUri);
                    rebuildRows(); });
  lay->addWidget(addBtn);
  lay->addLayout(rows_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  lay->addWidget(buttons);
  rebuildRows();
}

void BusFxDialog::rebuildRows() {
  QLayoutItem* item;
  while ((item = rows_->takeAt(0)) != nullptr) {
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }
  misuc::project::Bus* bus = session_->findBus(busId_);
  if (!bus)
    return;
  for (const auto& fx : bus->fx) {
    const std::string fxId = fx.id;
    auto* wrap = new QWidget(this);
    auto* hl = new QHBoxLayout(wrap);
    hl->setContentsMargins(0, 0, 0, 0);

    auto* enable = new QCheckBox(wrap);
    enable->setChecked(fx.enabled);
    if (fx.uri == misuc::engine::kBuiltinCompUri)
      enable->setText(
          QStringLiteral("SC Compressor(key: %1)")
              .arg(fx.sidechainSource.empty()
                       ? QStringLiteral("self")
                       : QString::fromStdString(
                             session_->findBus(fx.sidechainSource)
                                 ? session_->findBus(fx.sidechainSource)->name
                                 : fx.sidechainSource)));
    else
      enable->setText(QString::fromStdString(fx.uri));
    hl->addWidget(enable, 1);

    auto* keyCombo = new QComboBox(wrap);
    keyCombo->addItem(QStringLiteral("Key: self"));
    int cur = 0;
    for (const auto& b : session_->buses()) {
      keyCombo->addItem(
          QStringLiteral("Key: %1").arg(QString::fromStdString(b.name)));
      if (b.id == (fx.uri == misuc::engine::kBuiltinCompUri
                       ? fx.sidechainSource
                       : std::string()))
        cur = keyCombo->count() - 1;
    }
    keyCombo->setCurrentIndex(cur);
    hl->addWidget(keyCombo);

    auto* remove = new QPushButton(QStringLiteral("×"), wrap);
    remove->setFixedSize(20, 20);
    remove->setCursor(Qt::PointingHandCursor);
    hl->addWidget(remove);

    connect(enable, &QCheckBox::toggled, this, [this, fxId](bool onn) {
      misuc::project::Bus* b = session_->findBus(busId_);
      for (auto& f : b->fx)
        if (f.id == fxId)
          f.enabled = onn;
    });
    connect(keyCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, fxId](int idx) {
              misuc::project::Bus* b = session_->findBus(busId_);
              for (auto& f : b->fx)
                if (f.id == fxId) {
                  if (idx <= 0) {
                    f.sidechainSource.clear();
                  } else if (idx - 1 <
                             static_cast<int>(session_->buses().size())) {
                    f.sidechainSource = session_->buses()[idx - 1].id;
                  }
                  return;
                }
            });
    connect(remove, &QPushButton::clicked, this, [this, fxId] {
      session_->removeBusFx(busId_, fxId);
      rebuildRows();
    });
    rows_->addWidget(wrap);
  }
}

} // namespace misuc::ui