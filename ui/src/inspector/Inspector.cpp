#include "Inspector.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "HintSystem.h"
#include "Theme.h"
#include "elide/ElideLabel.h"

namespace misuc::ui {

namespace {
// Registers a hint on a freshly created inspector control. No-op until the
// inspector is attached to the global HintManager.
void registerCtrl(HintManager* hints, QWidget* widget, const QString& name,
                  const QString& desc, const QString& shortcut = QString()) {
  if (!hints || !widget)
    return;
  hints->registerHint(widget, name, desc, shortcut);
}
}  // namespace

Inspector::Inspector(QWidget* parent) : QWidget(parent) {
  setMinimumSize(230, 200);
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(0);

  icon_ = new QLabel(this);
  icon_->setAlignment(Qt::AlignCenter);
  icon_->setContentsMargins(10, 10, 10, 0);
  layout_->addWidget(icon_);

  title_ = new ElideLabel(this);
  title_->setObjectName(QStringLiteral("inspectorTitle"));
  title_->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: 600;")
                            .arg(theme::kText.name()));
  title_->setAlignment(Qt::AlignCenter);
  title_->setContentsMargins(10, 4, 10, 8);
  layout_->addWidget(title_);

  // The scroll area holds all dynamic content (property grid, takes, FX rack)
  // so it never overflows the dock at small window sizes or with long FX chains.
  scrollArea_ = new QScrollArea(this);
  scrollArea_->setWidgetResizable(true);
  scrollArea_->setFrameShape(QFrame::NoFrame);
  scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea_->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
  scrollContent_ = new QWidget;
  scrollContent_->setStyleSheet(QStringLiteral("background: transparent;"));
  scrollLayout_ = new QVBoxLayout(scrollContent_);
  scrollLayout_->setContentsMargins(10, 0, 10, 10);
  scrollLayout_->setSpacing(4);
  scrollArea_->setWidget(scrollContent_);
  layout_->addWidget(scrollArea_, 1);

  showEmpty("Browse a workspace to inspect its selection.");
}

void Inspector::rebuildGrid() {
  clearSection();
  gridHost_ = new QWidget(scrollContent_);
  grid_ = new QGridLayout(gridHost_);
  grid_->setContentsMargins(0, 0, 0, 0);
  grid_->setHorizontalSpacing(10);
  grid_->setVerticalSpacing(6);
  scrollLayout_->insertWidget(0, gridHost_);
}

void Inspector::clearSection() {
  // The grid host owns every row widget, so rebuilding the grid deletes the
  // old cells outright; otherwise stale rows keep painting over the new ones.
  if (gridHost_) {
    delete gridHost_;
    gridHost_ = nullptr;
    grid_ = nullptr;
  }
  if (takesHost_) {
    delete takesHost_;
    takesHost_ = nullptr;
  }
  // The FX rack widgets are owned by fxHost_. Edits are pushed synchronously
  // (editRequest -> MainWindow rebuilds this inspector from inside the
  // emitting widget's own signal), so freeing the host here would delete a
  // widget while its event is still unwinding and crash Qt's follow-up
  // accessibility update. Defer the destruction to the next event-loop pass.
  if (fxHost_) {
    fxHost_->deleteLater();
    fxHost_ = nullptr;
  }
}

void Inspector::addRow(const QString& label, const QString& hint) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new ElideLabel(gridHost_);
  lab->setStyleSheet(QString("color: %1;")
                         .arg(theme::kTextDim.name()));
  auto* val = new ElideLabel(gridHost_);
  val->setStyleSheet(QString("color: %1;")
                         .arg(hint.isEmpty() ? theme::kTextFaint.name()
                                             : theme::kText.name()));
  val->setAlignment(Qt::AlignRight);
  lab->setText(label);
  val->setText(hint.isEmpty() ? "-" : hint);
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(val, row, 1);
}

void Inspector::addTextEdit(const QString& label, const QString& value,
                            const QString& property, const QString& signalTarget) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, gridHost_);
  lab->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
  auto* edit = new QLineEdit(value, gridHost_);
  edit->setStyleSheet(QString("background: %1; color: %2; border: 1px solid %3;"
                              " border-radius: 3px; padding: 2px 6px;")
                          .arg(theme::kChromeRaised.name(), theme::kText.name(),
                               theme::kChromeBorder.name()));
  connect(edit, &QLineEdit::editingFinished, this,
          [this, property, edit, signalTarget] {
            if (edit->text() != editorValue_[signalTarget]) {
              editorValue_[signalTarget] = edit->text();
              emit editRequest(property, edit->text());
            }
          });
  const bool trackField = label.compare(QStringLiteral("Track"), Qt::CaseInsensitive) == 0;
  registerCtrl(hints_, edit, QStringLiteral("Name"),
               trackField ? QStringLiteral("Rename this track")
                          : QStringLiteral("Rename the selection"));
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(edit, row, 1);
}

void Inspector::addToggle(const QString& label, bool on,
                          const QString& property) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, gridHost_);
  lab->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
  auto* btn = new QPushButton(on ? "ON" : "off", gridHost_);
  btn->setCheckable(true);
  btn->setChecked(on);
  btn->setStyleSheet(QString("QPushButton { background: %1; color: %2;"
                             " border: 1px solid %3; border-radius: 3px;"
                             " padding: 2px 6px; font-weight: 600; }"
                             " QPushButton:checked { background: %4;"
                             " color: %5; border-color: %4; }")
                         .arg(theme::kChromeRaised.name(), theme::kText.name(),
                              theme::kChromeBorder.name(), theme::kAccent.name(),
                              theme::kSurface.name()));
  connect(btn, &QPushButton::toggled, this,
          [this, property](bool on) {
            emit editRequest(property, on ? "1" : "0");
          });
  if (label.compare(QStringLiteral("Mute")) == 0)
    registerCtrl(hints_, btn, QStringLiteral("Mute"),
                 QStringLiteral("Silence this track in the mix"));
  else if (label.compare(QStringLiteral("Solo")) == 0)
    registerCtrl(hints_, btn, QStringLiteral("Solo"),
                 QStringLiteral("Isolate this track in the mix"));
  else
    registerCtrl(hints_, btn, label.trimmed(),
                 QStringLiteral("Toggle this option"));
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(btn, row, 1);
}

void Inspector::addNumber(const QString& label, double value, double min,
                          double max, const QString& property) {
  if (!grid_) rebuildGrid();
  int row = grid_->rowCount();
  auto* lab = new QLabel(label, gridHost_);
  lab->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
  auto* spin = new QDoubleSpinBox(gridHost_);
  spin->setRange(min, max);
  spin->setDecimals(property.contains("pan") ? 2 : 1);
  spin->setSingleStep(property.contains("pan") ? 0.1 : 0.5);
  if (property.contains("gain")) spin->setSuffix(" dB");
  if (property.contains("trim") || property.contains("duration"))
    spin->setSuffix(" b");
  spin->setValue(value);
  spin->setStyleSheet(QString("background: %1; color: %2; border: 1px solid %3;"
                              " border-radius: 3px; padding: 2px 4px;")
                          .arg(theme::kChromeRaised.name(), theme::kText.name(),
                               theme::kChromeBorder.name()));
  connect(spin, &QDoubleSpinBox::editingFinished, this,
          [this, property, spin] {
            emit editRequest(property, QString::number(spin->value(), 'f', 2));
          });
  if (label.compare(QStringLiteral("Gain")) == 0)
    registerCtrl(hints_, spin, QStringLiteral("Gain"),
                 QStringLiteral("Level in dB (-60 to 24)"));
  else if (label.compare(QStringLiteral("Pan")) == 0)
    registerCtrl(hints_, spin, QStringLiteral("Pan"),
                 QStringLiteral("Stereo position (left to right)"));
  else if (label.compare(QStringLiteral("Trim-in")) == 0)
    registerCtrl(hints_, spin, QStringLiteral("Trim-in"),
                 QStringLiteral("Playback offset into the source, in beats"));
  else
    registerCtrl(hints_, spin, label.trimmed(),
                 QStringLiteral("Numeric property of the selection"));
  grid_->addWidget(lab, row, 0);
  grid_->addWidget(spin, row, 1);
}

void Inspector::showTrack(int row, const QString& name, bool muted, bool solo,
                          float gainDb, float pan,
                          const std::vector<FxSlotView>& fxSlots,
                          const std::vector<FxCatalogItem>& catalog,
                          const std::vector<TakeView>& takes) {
  rebuildGrid();
  title_->setText(name.isEmpty() ? QStringLiteral("[Track %1]").arg(row)
                                  : name);
  if (name.isEmpty())
    addRow("Track", QString::number(row + 1));
  else
    addTextEdit("Track", name, "track.name", "track.name");
  addToggle("Mute", muted, "track.mute");
  addToggle("Solo", solo, "track.solo");
  addNumber("Gain", gainDb, -60.0, 24.0, "track.gain");
  addNumber("Pan", static_cast<double>(pan), -1.0, 1.0, "track.pan");
  addTakes(takes);
  addFxRack(fxSlots, catalog);
}

void Inspector::addTakes(const std::vector<TakeView>& takes) {
  takesHost_ = new QWidget(this);
  auto* fl = new QVBoxLayout(takesHost_);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(4);

  auto* head = new QLabel(QStringLiteral("TAKES"), takesHost_);
  head->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 700;")
                          .arg(theme::kTextDim.name()));
  fl->addWidget(head);

  if (takes.empty()) {
    auto* none = new QLabel(
        QStringLiteral("Record onto this track to build a take list"),
        takesHost_);
    none->setStyleSheet(
        QString("color: %1;").arg(theme::kTextFaint.name()));
    none->setWordWrap(true);
    fl->addWidget(none);
  } else {
    for (const TakeView& tk : takes) {
      auto* row = new QWidget(takesHost_);
      auto* rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->setSpacing(6);
      auto* name = new QLabel(tk.name, row);
      name->setToolTip(QStringLiteral("%1 at beat %2, %3 beats long")
                           .arg(tk.name)
                           .arg(tk.startBeats, 0, 'f', 1)
                           .arg(tk.durationBeats, 0, 'f', 1));
      name->setStyleSheet(QString("color: %1; font-weight: 600;")
                              .arg(theme::kText.name()));
      auto* place = new QPushButton(tk.placed ? "ACTIVE" : "place", row);
      place->setObjectName(QStringLiteral("takePlace_") + tk.id);
      place->setCheckable(false);
      place->setStyleSheet(
          QString("QPushButton { background: %1; color: %2;"
                  " border: 1px solid %3; border-radius: 3px;"
                  " padding: 2px 6px; font-weight: 600; }")
              .arg(theme::kChromeRaised.name(), theme::kText.name(),
                   theme::kChromeBorder.name()));
const QString prop = QStringLiteral("take.%2").arg(tk.id);
    connect(place, &QPushButton::clicked, this,
            [this, prop] { emit editRequest(prop + ".place", "1"); });
    registerCtrl(hints_, place, QStringLiteral("Place Take"),
                 QStringLiteral("Collapse the timeline to this take"));
      auto* del = new QPushButton(QStringLiteral("x"), row);
      del->setObjectName(QStringLiteral("takeDel_") + tk.id);
      del->setToolTip(QStringLiteral("Delete this take and its placed clip"));
      del->setFixedSize(22, 22);
      del->setStyleSheet(
          QString("QPushButton { background: transparent; color: %1;"
                  " border: 1px solid %2; border-radius: 3px;"
                  " font-weight: 700; padding: 0px; }"
                  " QPushButton:hover { color: %3; border-color: %3; }")
              .arg(theme::kTextFaint.name(), theme::kChromeBorder.name(),
                   theme::kAccent.name()));
connect(del, &QPushButton::clicked, this,
            [this, prop] { emit editRequest(prop + ".remove", "1"); });
    registerCtrl(hints_, del, QStringLiteral("Delete Take"),
                 QStringLiteral("Remove this take and its placed clip"),
                 QStringLiteral("Del"));
      rl->addWidget(name, 1);
      rl->addWidget(place);
      rl->addWidget(del);
      fl->addWidget(row);
    }
  }

  scrollLayout_->addWidget(takesHost_);
}

void Inspector::addFxRack(const std::vector<FxSlotView>& fxSlots,
                          const std::vector<FxCatalogItem>& catalog) {
  auto* host = new QWidget(this);
  auto* fl = new QVBoxLayout(host);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(4);

  auto* head = new QLabel(QStringLiteral("EFFECTS"), host);
  head->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 700;")
                          .arg(theme::kTextDim.name()));
  fl->addWidget(head);

  const auto buttonStyle = [](bool enabled) {
    return QString("QPushButton { background: %1; color: %2;"
                   " border: 1px solid %3; border-radius: 3px;"
                   " padding: 2px 6px; font-weight: 600; }"
                   " QPushButton:checked { background: %4; color: %5;"
                   " border-color: %4; }")
        .arg(theme::kChromeRaised.name(), theme::kText.name(),
             theme::kChromeBorder.name(), theme::kAccent.name(),
             enabled ? theme::kSurface.name() : theme::kTextDim.name());
  };
  // Small "A" arm toggle shown next to each parameter: arming creates/writes an
  // "fx:<chain>:<port>" automation lane (the playlist band then records the
  // control's live value while transport plays).
  const auto armStyle = [](bool armed) {
    return QString(
               "QPushButton { background: %1; color: %2; border: 1px solid %3;"
               " border-radius: 3px; font-weight: 700; padding: 0px; }"
               " QPushButton:checked { background: %4; color: %5;"
               " border-color: %4; }")
        .arg(armed ? theme::kAccent.name() : theme::kChromeRaised.name(),
             armed ? theme::kSurface.name() : theme::kTextDim.name(),
             theme::kChromeBorder.name(), theme::kDanger.name(),
             theme::kSurface.name());
  };

  if (fxSlots.empty()) {
    auto* none = new QLabel(QStringLiteral("No effects on this track"), host);
    none->setStyleSheet(
        QString("color: %1;").arg(theme::kTextFaint.name()));
    fl->addWidget(none);
  } else {
    for (const FxSlotView& s : fxSlots) {
      auto* slotRow = new QWidget(host);
      auto* rl = new QHBoxLayout(slotRow);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->setSpacing(6);
      auto* name = new QLabel(s.name, slotRow);
      name->setStyleSheet(QString("color: %1; font-weight: 600;")
                              .arg(theme::kText.name()));
      auto* onBtn = new QPushButton(s.enabled ? "ON" : "off", slotRow);
      onBtn->setObjectName(QStringLiteral("fxOn_") + s.id);
      onBtn->setCheckable(true);
      onBtn->setChecked(s.enabled);
      onBtn->setStyleSheet(buttonStyle(s.enabled));
      const QString slotProp = QStringLiteral("fx.%2").arg(s.id);
      connect(onBtn, &QPushButton::toggled, this,
              [this, slotProp](bool on) {
                emit editRequest(slotProp + ".enable", on ? "1" : "0");
              });
      registerCtrl(hints_, onBtn, QStringLiteral("Enable Insert"),
                   QStringLiteral("Toggle this insert in and out of the chain"));
      auto* del = new QPushButton(QStringLiteral("x"), slotRow);
      del->setObjectName(QStringLiteral("fxDel_") + s.id);
      del->setToolTip(QStringLiteral("Remove this insert"));
      del->setFixedSize(22, 22);
      del->setStyleSheet(
          QString("QPushButton { background: transparent; color: %1;"
                  " border: 1px solid %2; border-radius: 3px;"
                  " font-weight: 700; padding: 0px; }"
                  " QPushButton:hover { color: %3; border-color: %3; }")
              .arg(theme::kTextFaint.name(), theme::kChromeBorder.name(),
                   theme::kAccent.name()));
      connect(del, &QPushButton::clicked, this, [this, slotProp] {
        emit editRequest(slotProp + ".remove", "1");
      });
      registerCtrl(hints_, del, QStringLiteral("Remove Insert"),
                   QStringLiteral("Remove this insert from the chain"),
                   QStringLiteral("Del"));
      rl->addWidget(name, 1);
      rl->addWidget(onBtn);
      rl->addWidget(del);
      fl->addWidget(slotRow);

      for (const FxSlotView::Param& p : s.params) {
        double lo = p.min;
        double hi = p.max;
        if (hi < lo) std::swap(lo, hi);
        if (hi == lo) hi = lo + 1.0;
        auto* prow = new QWidget(host);
        auto* pl = new QHBoxLayout(prow);
        pl->setContentsMargins(0, 0, 0, 0);
        pl->setSpacing(6);
        auto* lab = new QLabel(p.label, prow);
        lab->setStyleSheet(
            QString("color: %1;").arg(theme::kTextDim.name()));
        auto* spin = new QDoubleSpinBox(prow);
        spin->setObjectName(QStringLiteral("fxParam_") + s.id + QStringLiteral("_") +
                            QString::number(p.index));
        spin->setRange(lo, hi);
        spin->setDecimals(4);
        spin->setValue(qBound(lo, p.value, hi));
        spin->setStyleSheet(
            QString("background: %1; color: %2; border: 1px solid %3;"
                    " border-radius: 3px; padding: 2px 4px;")
                .arg(theme::kChromeRaised.name(), theme::kText.name(),
                     theme::kChromeBorder.name()));
        connect(spin, &QDoubleSpinBox::editingFinished, this,
                [this, slotProp, index = p.index, spin] {
                  emit editRequest(
                      slotProp + QStringLiteral(".param.%1").arg(index),
                      QString::number(spin->value(), 'f', 4));
                });
        auto* arm = new QPushButton(QStringLiteral("A"), prow);
        arm->setObjectName(QStringLiteral("fxArm_") + s.id +
                           QStringLiteral("_") + QString::number(p.index));
        arm->setToolTip(
            QStringLiteral("Automate this parameter: arm creates the fx "
                           "automation lane and records its value while "
                           "transport plays"));
        arm->setCheckable(true);
        arm->setChecked(p.armed);
        arm->setFixedSize(22, 22);
        arm->setStyleSheet(armStyle(p.armed));
        connect(arm, &QPushButton::toggled, this,
                [this, slotProp, index = p.index](bool on) {
                  emit editRequest(
                      slotProp + QStringLiteral(".auto.%1").arg(index),
                      on ? QStringLiteral("1") : QStringLiteral("0"));
                });
        registerCtrl(hints_, arm, QStringLiteral("Fx Automation"),
                     QStringLiteral("Arm this parameter's automation lane"));
        pl->addWidget(lab, 1);
        pl->addWidget(spin);
        pl->addWidget(arm);
        registerCtrl(hints_, spin, QStringLiteral("FX Parameter"),
                     QStringLiteral("Edit this insert's native parameter"),
                     QString());
        fl->addWidget(prow);
      }
    }
  }

  if (!catalog.empty()) {
    auto* addRow = new QWidget(host);
    auto* al = new QHBoxLayout(addRow);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(6);
    auto* combo = new QComboBox(addRow);
    combo->setObjectName(QStringLiteral("fxAddCombo"));
    combo->setStyleSheet(QString("background: %1; color: %2;"
                                 " border: 1px solid %3;"
                                 " border-radius: 3px; padding: 2px 4px;")
                             .arg(theme::kChromeRaised.name(),
                                  theme::kText.name(),
                                  theme::kChromeBorder.name()));
    for (const FxCatalogItem& item : catalog)
      combo->addItem(item.name, item.uri);
    auto* addBtn = new QPushButton(QStringLiteral("Add"), addRow);
    addBtn->setObjectName(QStringLiteral("fxAddBtn"));
    addBtn->setStyleSheet(
        QString("QPushButton { background: %1; color: %2;"
                " border: 1px solid %1; border-radius: 3px;"
                " padding: 2px 8px; font-weight: 600; }")
            .arg(theme::kAccent.name(), theme::kSurface.name()));
    connect(addBtn, &QPushButton::clicked, this, [this, combo] {
      const QString uri = combo->currentData().toString();
      if (!uri.isEmpty())
        emit editRequest("fx.add", uri);
    });
    registerCtrl(hints_, combo, QStringLiteral("Add Insert"),
                 QStringLiteral("Pick an effect from the catalog"));
    registerCtrl(hints_, addBtn, QStringLiteral("Add Insert"),
                 QStringLiteral("Add the selected effect to the chain"));
    al->addWidget(combo, 1);
    al->addWidget(addBtn);
    fl->addWidget(addRow);
  } else {
    auto* none = new QLabel(
        QStringLiteral("LV2 hosting unavailable - FX cannot be added"),
        host);
    none->setStyleSheet(
        QString("color: %1;").arg(theme::kTextFaint.name()));
    none->setWordWrap(true);
    fl->addWidget(none);
  }

  fxHost_ = host;
  scrollLayout_->addWidget(fxHost_);
}

void Inspector::showAudioClip(const QString& name, const QString& source,
                              double startBeats, double durationBeats,
                              float gainDb, double clipOffsetBeats) {
  rebuildGrid();
  title_->setText(name);
  addTextEdit("Name", name, "clip.name", "clip.name");
  addRow("Source", source);
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
  addNumber("Trim-in", clipOffsetBeats, 0.0, 100000.0, "clip.trim");
  addNumber("Gain", gainDb, -60.0, 24.0, "clip.gain");
}

void Inspector::showMidiRegion(int noteCount, double startBeats,
                               double durationBeats) {
  rebuildGrid();
  title_->setText("MIDI Region");
  addRow("Notes", QString::number(noteCount));
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
}

void Inspector::showNote(int pitch, float velocity, double startBeats,
                         double durationBeats) {
  rebuildGrid();
  title_->setText("Note");
  addRow("Pitch", QString::number(pitch));
  addRow("MIDI", QString::number(pitch));
  addRow("Velocity", QString::number(qRound(velocity * 127)));
  addRow("Start", QString::number(startBeats, 'f', 2) + " b");
  addRow("Length", QString::number(durationBeats, 'f', 2) + " b");
}

void Inspector::showEmpty(const QString& workspaceHint) {
  rebuildGrid();
  title_->setText("Inspector");
  addRow("Context", workspaceHint);
}

void Inspector::clear() {
  title_->setText("No selection");
  clearSection();
}

void Inspector::attachHints(HintManager* hints) {
  hints_ = hints;
  // The per-control hints are registered eagerly by the add* helpers when the
  // property grid is rebuilt, so nothing further is needed here beyond holding
  // the registry pointer.
}

} // namespace misuc::ui