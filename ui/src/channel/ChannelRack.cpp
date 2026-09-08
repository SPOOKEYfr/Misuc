#include "ChannelRack.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QSlider>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QToolButton>

#include "HintSystem.h"
#include "Theme.h"
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

#include "Theme.h"
#include "branding/Icons.h"

namespace misuc::ui {

namespace {

constexpr int kStepCount = 16;

class RackDelegate : public QStyledItemDelegate {
public:
  QVector<ChannelRack::Channel>* channels;
  int stepBase;
  ChannelRack* rack;

  void paint(QPainter* p, const QStyleOptionViewItem& opt,
             const QModelIndex& index) const override {
    const int col = index.column();
    const int row = index.row();
    ChannelRack::Channel& ch = (*channels)[row];

    if (col >= stepBase && col < stepBase + kStepCount) {
      bool on = ch.steps[rack->pattern()][col - stepBase];
      p->fillRect(opt.rect, theme::kPanelSunken);
      QRectF cell(opt.rect.x() + 1.5, opt.rect.y() + 1.5,
                  opt.rect.width() - 3, opt.rect.height() - 3);
      if (on) {
        p->setPen(Qt::NoPen);
        p->setBrush(theme::kAccent);
        p->drawRoundedRect(cell, 3, 3);
      } else {
        p->setBrush(theme::kGridRow);
        p->setPen(QColor(0, 0, 0, 0));
        p->drawRoundedRect(cell, 3, 3);
      }
      // 16-step grouping: a soft divider after every 4 steps.
      int stepIn = col - stepBase;
      if (stepIn > 0 && stepIn % 4 == 0) {
        p->setPen(QPen(QColor(theme::kChromeBorder.red(),
                              theme::kChromeBorder.green(),
                              theme::kChromeBorder.blue(), 90),
                       1));
        p->drawLine(opt.rect.topLeft(), opt.rect.bottomLeft());
      }
      if (opt.state.testFlag(QStyle::State_Selected)) {
        p->setPen(theme::kAccent);
        p->drawRect(opt.rect.adjusted(0, 0, -1, -1));
      }
      return;
    }

    p->fillRect(opt.rect, opt.state.testFlag(QStyle::State_Selected)
                        ? theme::kAccentSoft : theme::kPanel);
    p->setPen(theme::kText);
    QFont f = p->font(); f.setPixelSize(12); p->setFont(f);

    if (col == 0) {
      p->setPen(theme::kText);
      p->drawText(opt.rect.adjusted(8,0,0,0), Qt::AlignVCenter | Qt::AlignLeft,
                  ch.name);
      return;
    }
    if (col == 1) {
      // mute
      auto r = opt.rect.adjusted(2,3,-2,-3);
      p->setPen(Qt::NoPen);
      p->setBrush(ch.muted ? theme::kDangerSoft : theme::kGridRow);
      p->drawRoundedRect(r, 4, 4);
      p->setPen(Qt::NoPen);
      p->setBrush(ch.muted ? theme::kDanger : theme::kTextFaint);
      p->drawRect(QRectF(r.center().x()-6.5, r.center().y()-0.5, 13, 1));
      return;
    }
    if (col == 2) {
      auto r = opt.rect.adjusted(2,3,-2,-3);
      p->setPen(Qt::NoPen);
      p->setBrush(ch.solo ? theme::kAccent : theme::kGridRow);
      p->drawRoundedRect(r, 4, 4);
      p->setPen(QColor(0,0,0,150));
      QFont sf = p->font(); sf.setBold(true); sf.setPixelSize(11); p->setFont(sf);
      p->drawText(r, Qt::AlignCenter, "S");
      p->setFont(f);
      return;
    }
    if (col == stepBase + kStepCount) {
      // volume bar
      p->setPen(Qt::NoPen);
      p->setBrush(theme::kGridRow);
      p->drawRect(opt.rect.x()+4, opt.rect.y()+opt.rect.height()/2-4,
                  opt.rect.width()-8, 8);
      p->setBrush(theme::kAccent);
      p->drawRect(opt.rect.x()+4, opt.rect.y()+opt.rect.height()/2-4,
                  int((opt.rect.width()-8) * ch.volume), 8);
      p->setPen(theme::kText);
      p->drawText(opt.rect, Qt::AlignCenter | Qt::AlignVCenter,
                  QString::number(int(ch.volume*100)) + '%');
      return;
    }
    if (col == stepBase + kStepCount + 1) {
      // pan text
      QString t = (ch.pan < -0.01) ? "L " : ((ch.pan > 0.01) ? "R " : "C ");
      t += QString::number(int(fabs(ch.pan) * 100));
      p->setPen(theme::kTextDim);
      p->drawText(opt.rect, Qt::AlignCenter, t);
      return;
    }
    QStyledItemDelegate::paint(p, opt, index);
  }

  QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override {
    auto s = QStyledItemDelegate::sizeHint(o, i);
    s.setHeight(26);
    return s;
  }

  bool editorEvent(QEvent* ev, QAbstractItemModel* model,
                   const QStyleOptionViewItem& opt, const QModelIndex& index) override {
    if (ev->type() != QEvent::MouseButtonPress &&
        ev->type() != QEvent::MouseButtonDblClick)
      return QStyledItemDelegate::editorEvent(ev, model, opt, index);
    if (index.column() >= stepBase && index.column() < stepBase + kStepCount) {
      ChannelRack::Channel& ch = (*channels)[index.row()];
      QVector<bool>& bank = ch.steps[rack->pattern()];
      bank[index.column() - stepBase] = !bank[index.column() - stepBase];
      model->dataChanged(index, index);
      Q_EMIT rack->edited();
      return true;
    }
    if (index.column() == 1) {
      ChannelRack::Channel& ch = (*channels)[index.row()];
      ch.muted = !ch.muted;
      model->dataChanged(index, index);
      Q_EMIT rack->edited();
      return true;
    }
    if (index.column() == 2) {
      ChannelRack::Channel& ch = (*channels)[index.row()];
      ch.solo = !ch.solo;
      model->dataChanged(index, index);
      Q_EMIT rack->edited();
      return true;
    }
    return QStyledItemDelegate::editorEvent(ev, model, opt, index);
  }
};

} // namespace

ChannelRack::ChannelRack(QWidget* parent) : QWidget(parent) {
  setMinimumSize(560, 260);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // ── icon-only chrome header (matte glass, §36-38/§66-68)
  auto* chrome = new QWidget(this);
  chrome->setObjectName(QStringLiteral("rackChrome"));
  chrome->setFixedHeight(34);
  chrome->setStyleSheet(
      QStringLiteral("QWidget#rackChrome { background: %1; "
                     "border-bottom: 1px solid %2; }")
          .arg(theme::kChrome.name(QColor::HexArgb),
               theme::kChromeBorder.name(QColor::HexArgb)));
  auto* cl = new QHBoxLayout(chrome);
  cl->setContentsMargins(10, 3, 10, 3);
  cl->setSpacing(4);

  auto mkChromeBtn = [&](icons::IconId id, const QString& tip) {
    auto* b = new QToolButton(chrome);
    b->setIcon(icons::make(id, theme::kTextDim, 16));
    b->setIconSize(QSize(16, 16));
    b->setAutoRaise(true);
    b->setFixedSize(26, 26);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    chromeButtons_.push_back(b);
    return b;
  };

  auto* title = new QLabel(QStringLiteral("CHANNEL RACK"), chrome);
  title->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; "
                                      "font-weight: 800; letter-spacing: 2px;")
                           .arg(theme::kTextMuted.name()));
  cl->addWidget(title);

  // ── pattern selector: four 16-step banks, icon-first numbered toggles.
  for (int i = 0; i < kPatternCount; ++i) {
    auto* b = new QToolButton(chrome);
    b->setText(QString::number(i + 1));
    b->setCheckable(true);
    b->setChecked(i == 0);
    b->setAutoRaise(true);
    b->setFixedSize(24, 24);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(
        QStringLiteral("QToolButton { color: %1; font-size: 10px; "
                       "font-weight: 700; border: 1px solid %2; "
                       "border-radius: 5px; background: %3; } "
                       "QToolButton:checked { color: %4; border-color: %5; "
                       "background: %6; }")
            .arg(theme::kTextDim.name(), theme::kChromeBorder.name(),
                 theme::kChromeRaised.name(QColor::HexArgb),
                 theme::kAccent.name(), theme::kAccent.name(),
                 theme::kAccentSoft.name()));
    patternButtons_.push_back(b);
    cl->addWidget(b);
    const int bank = i;
    connect(b, &QToolButton::clicked, this,
            [this, bank](bool) { setPattern(bank); });
  }

  cl->addSpacing(6);

  // ── swing control (0..66%): groove amount for the active pattern bank.
  swingSlider_ = new QSlider(Qt::Horizontal, chrome);
  swingSlider_->setRange(0, kMaxSwing);
  swingSlider_->setSingleStep(1);
  swingSlider_->setFixedWidth(92);
  swingSlider_->setFixedHeight(24);
  swingSlider_->setFocusPolicy(Qt::NoFocus);
  swingSlider_->setCursor(Qt::PointingHandCursor);
  connect(swingSlider_, &QSlider::valueChanged, this,
          [this](int v) { setSwing(v); });
  cl->addWidget(swingSlider_);

  cl->addStretch(1);

  auto* prevBtn = mkChromeBtn(icons::IconId::Preview, "Preview sound (row)");
  auto* addBtn = mkChromeBtn(icons::IconId::Add, "Add channel");
  auto* dupBtn = mkChromeBtn(icons::IconId::Duplicate, "Duplicate channel");
  auto* delBtn = mkChromeBtn(icons::IconId::Remove, "Remove channel");
  auto* resetBtn = mkChromeBtn(icons::IconId::Undo, "Reset to starter kit");
  cl->addWidget(prevBtn);
  cl->addSpacing(4);
  cl->addWidget(addBtn);
  cl->addWidget(dupBtn);
  cl->addWidget(delBtn);
  cl->addSpacing(4);
  cl->addWidget(resetBtn);
  cl->addSpacing(4);
  placePatternBtn_ = new QToolButton(chrome);
  placePatternBtn_->setToolTip(QStringLiteral(
      "Place Pattern: turn this bank's steps into a real MIDI region in the "
      "arrangement (editable in piano roll)"));
  placePatternBtn_->setIcon(icons::make(icons::IconId::Patterns, theme::kTextDim, 16));
  placePatternBtn_->setAutoRaise(true);
  placePatternBtn_->setFixedSize(26, 26);
  placePatternBtn_->setCursor(Qt::PointingHandCursor);
  placePatternBtn_->setFocusPolicy(Qt::NoFocus);
  connect(placePatternBtn_, &QToolButton::clicked, this,
          &ChannelRack::placePattern);
  cl->addWidget(placePatternBtn_);
  cl->addSpacing(4);
  patternPlayBtn_ = new QToolButton(chrome);
  patternPlayBtn_->setObjectName(QStringLiteral("patternPlay"));
  patternPlayBtn_->setToolTip(QStringLiteral(
      "Pattern Play: loop this bank through the audio engine"));
  patternPlayBtn_->setCheckable(true);
  patternPlayBtn_->setIcon(icons::make(icons::IconId::Play, theme::kTextDim, 16));
  patternPlayBtn_->setStyleSheet(
      QStringLiteral("QToolButton { color: %1; } "
                     "QToolButton:checked { color: %2; border: 1px solid %3; "
                     "border-radius: 3px; background: %4; }")
          .arg(theme::kTextDim.name(), theme::kAccentActive.name(),
               theme::kAccent.name(), theme::kAccentSoft.name()));
  connect(patternPlayBtn_, &QToolButton::toggled, this, [this](bool on) {
    Q_EMIT patternLiveChanged(on);
  });
  cl->addWidget(patternPlayBtn_);
  lay->addWidget(chrome);

  view_ = new QTableView(this);
  view_->setSelectionBehavior(QAbstractItemView::SelectItems);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::DoubleClicked |
                         QAbstractItemView::EditKeyPressed);
  view_->setFocusPolicy(Qt::NoFocus);
  view_->horizontalHeader()->setStretchLastSection(true);
  view_->verticalHeader()->setVisible(false);
  view_->horizontalHeader()->setVisible(true);
  view_->horizontalHeader()->setDefaultSectionSize(26);
  lay->addWidget(view_);

  model_ = new QStandardItemModel(this);
  view_->setModel(model_);

  auto* del = new RackDelegate;
  del->channels = &channels_;
  del->stepBase = stepBase_;
  del->rack = this;
  view_->setItemDelegate(del);
  (void)del;

  loadPersisted();
  if (channels_.isEmpty()) restoreDefaults();
  rebuildTable();
  if (swingSlider_) swingSlider_->setValue(swing_);
  for (int i = 0; i < patternButtons_.size(); ++i)
    patternButtons_[i]->setChecked(i == pattern_);

  connect(view_, &QTableView::clicked, this, [this](const QModelIndex& i) {
    setCurrent(i.row());
    Q_EMIT channelSelected(i.row());
  });

  // Chrome actions. Need to keep chromeButtons_ pointers local: buttons are
  // [preview, add, duplicate, remove, reset] in creation order.
  connect(chromeButtons_[0], &QToolButton::clicked, this,
          &ChannelRack::previewCurrent);
  connect(chromeButtons_[1], &QToolButton::clicked, this,
          [this](bool) { addChannel(); });
  connect(chromeButtons_[2], &QToolButton::clicked, this,
          &ChannelRack::duplicateCurrent);
  connect(chromeButtons_[3], &QToolButton::clicked, this,
          &ChannelRack::removeCurrent);
  connect(chromeButtons_[4], &QToolButton::clicked, this,
          &ChannelRack::resetToDefaults);

  view_->setStyleSheet(
      QStringLiteral(
          "QTableView { background: %1; alternate-background-color: %2; "
          "border: none; gridline-color: %3; selection-background-color: %4; "
          "selection-color: %5; color: %6; } "
          "QHeaderView::section { background: %7; color: %8; border: none; "
          "border-bottom: 1px solid %9; padding: 3px 4px; font-size: 9px; "
          "font-weight: 600; }")
          .arg(theme::kSurface.name(QColor::HexArgb),
               theme::kPanel.name(QColor::HexArgb),
               theme::kChromeBorder.name(QColor::HexArgb),
               theme::kAccentSoft.name(), theme::kAccentActive.name(),
               theme::kText.name(), theme::kChromeRaised.name(QColor::HexArgb),
               theme::kTextDim.name(),
               theme::kChromeBorder.name(QColor::HexArgb)));
}

double ChannelRack::previewFreq(int row) const {
  if (row >= 0 && row < 5)
    return previewPitch_[row];
  return 220.0;
}

bool ChannelRack::patternLive() const {
  return patternPlayBtn_ && patternPlayBtn_->isChecked();
}

QString ChannelRack::channelName(int row) const {
  if (row < 0 || row >= channels_.size())
    return QString();
  return channels_[row].name;
}

void ChannelRack::previewCurrent() {
  if (!hasSelection())
    return;
  Q_EMIT previewRequested(current_);
}

void ChannelRack::placePattern() { Q_EMIT placePatternRequested(); }

void ChannelRack::resetToDefaults() {
  restoreDefaults();
  rebuildTable();
  Q_EMIT edited();
}

void ChannelRack::attachHints(HintManager* hints) {
  if (!hints)
    return;
  hints->registerTool(chromeButtons_[0], QStringLiteral("Preview Channel"),
                      QStringLiteral("Audition the selected channel's sound "
                                     "through the engine voice"), QString(), icons::IconId::Preview,
                      [this] {
                        if (!hasSelection())
                          return QStringLiteral("no selection");
                        return QStringLiteral("%1 · %2 Hz")
                            .arg(channelName(current_))
                            .arg(previewFreq(current_), 0, 'f', 0);
                      });
  hints->registerTool(chromeButtons_[1], QStringLiteral("Add Channel"),
                      QStringLiteral("Append a new step-sequencer channel"), QString(), icons::IconId::Add);
  hints->registerTool(chromeButtons_[2], QStringLiteral("Duplicate Channel"),
                      QStringLiteral("Duplicate the selected channel with its "
                                     "steps and levels"), QString(), icons::IconId::Duplicate);
  hints->registerTool(chromeButtons_[3], QStringLiteral("Remove Channel"),
                      QStringLiteral("Delete the selected channel"), QString(), icons::IconId::Remove);
  hints->registerTool(chromeButtons_[4], QStringLiteral("Reset to Starter Kit"),
                      QStringLiteral("Restore the Kick/Snare/Hi-Hat/Bass/Sampler "
                                     "starter lineup"), QString(), icons::IconId::Undo);
  hints->registerTool(patternPlayBtn_, QStringLiteral("Pattern Play"),
                      QStringLiteral("Loop this pattern bank through the audio "
                                     "engine (mute/solo/volume and swing are "
                                     "honoured in real time)"),
                      QString(), icons::IconId::Play,
                      [this] { return patternLive() ? QStringLiteral("playing")
                                                   : QString(); });
  for (int i = 0; i < patternButtons_.size(); ++i) {
    const int bank = i;
    hints->registerTool(patternButtons_[i],
                        QStringLiteral("Pattern %1").arg(bank + 1),
                        QStringLiteral("Switch the step lanes to 16-step group %1 "
                                       "(persisted per pattern)").arg(bank + 1),
                        QString(), icons::IconId::Patterns,
                        [this, bank] {
                          return pattern_ == bank ? QStringLiteral("active")
                                                  : QString();
                        });
  }
  if (swingSlider_)
    hints->registerHint(swingSlider_, QStringLiteral("Swing"),
                        QStringLiteral("Groove swing for the step lanes, stored "
                                       "per session (0–66%). Off-beat 16ths are "
                                       "delayed toward the following step when "
                                       "Pattern Play is armed"),
                        QString(), [this] {
                          return QStringLiteral("%1%").arg(swing_);
                        });

  // Exact-cell hints over the table: name, mute, solo, step gates, volume and
  // pan all resolve from the cell under the cursor with live rack state.
  const QPointer<ChannelRack> self(this);
  hints->registerCustom(view_, [self](const QPoint& pos) -> std::optional<Hint> {
    if (self.isNull() || !self->view_)
      return std::nullopt;
    QWidget* vp = self->view_->viewport();
    const QPoint vpos = vp->mapFromParent(pos);
    if (!vp->rect().contains(vpos))
      return std::nullopt;
    const QModelIndex idx = self->view_->indexAt(vpos);
    if (!idx.isValid())
      return std::nullopt;
    const int row = idx.row();
    const int col = idx.column();
    if (row < 0 || row >= self->channels_.size())
      return std::nullopt;
    const ChannelRack::Channel& ch = self->channels_[row];
    const QString name = QString::fromStdString(ch.name.toStdString());
    Hint h;
    if (col == 0) {
      h.name = name;
      h.description = QStringLiteral("Step-sequencer channel · %1").arg(
          ch.muted ? QStringLiteral("muted") : QStringLiteral("live"));
      if (ch.muted) h.name += QStringLiteral(" (M)");
      return h;
    }
    if (col == 1) {
      h.name = name + QStringLiteral(" · Mute");
      h.description = QStringLiteral("Silence this channel's gates in Pattern Play");
      h.value = [self, row] {
        if (self.isNull() || row >= self->channels_.size())
          return QString();
        return self->channels_[row].muted ? QStringLiteral("muted")
                                          : QStringLiteral("off");
      };
      return h;
    }
    if (col == 2) {
      h.name = name + QStringLiteral(" · Solo");
      h.description = QStringLiteral("Isolate this channel in the step pattern");
      h.value = [self, row] {
        if (self.isNull() || row >= self->channels_.size())
          return QString();
        return self->channels_[row].solo ? QStringLiteral("solo")
                                         : QStringLiteral("off");
      };
      return h;
    }
    if (col >= self->stepBase_ && col < self->stepBase_ + kStepCount) {
      const int step = col - self->stepBase_;
      h.name = name + QStringLiteral(" · Step %1").arg(step + 1);
      h.description = QStringLiteral("Gate a one-sixteenth step of pattern %1")
                          .arg(self->pattern_ + 1);
      h.value = [self, row, step] {
        if (self.isNull() || row >= self->channels_.size())
          return QString();
        const ChannelRack::Channel& c = self->channels_[row];
        const int bank = self->pattern_;
        const bool on =
            step < c.steps[bank].size() && c.steps[bank][step];
        return on ? QStringLiteral("gated") : QStringLiteral("off");
      };
      return h;
    }
    if (col == self->stepBase_ + kStepCount) {
      h.name = name + QStringLiteral(" · Volume");
      h.description = QStringLiteral("Channel level heard through the engine");
      h.value = [self, row] {
        if (self.isNull() || row >= self->channels_.size())
          return QString();
        return QStringLiteral("%1%").arg(
            int(self->channels_[row].volume * 100.f));
      };
      return h;
    }
    if (col == self->stepBase_ + kStepCount + 1) {
      h.name = name + QStringLiteral(" · Pan");
      h.description = QStringLiteral("Stereo pan of the channel");
      h.value = [self, row] {
        if (self.isNull() || row >= self->channels_.size())
          return QString();
        const float p = self->channels_[row].pan;
        const QString d = p < -0.01f ? QStringLiteral("L") :
                          p > 0.01f  ? QStringLiteral("R")
                                     : QStringLiteral("C");
        return d + QString::number(int(std::fabs(p) * 100.f));
      };
      return h;
    }
    return std::nullopt;
  });
}

void ChannelRack::restoreDefaults() {
  channels_.clear();
  struct Seed { const char* n; float v; float pan; const char* steps; };
  const Seed seeds[] = {
    {"Kick",   0.90f, 0.0f, "1000100010001000"},
    {"Snare",  0.85f, 0.0f, "0000100000001000"},
    {"Hi-Hat", 0.55f, 0.0f, "1010101010101010"},
    {"Bass",   0.80f, 0.0f, "1000001000000000"},
    {"Sampler",0.70f, 0.0f, "1000000010000000"},
  };
  for (auto& s : seeds) {
    Channel c;
    c.name = QString::fromLatin1(s.n);
    c.volume = s.v;
    c.pan = s.pan;
    for (int b = 0; b < kPatternCount; ++b)
      c.steps[b].fill(false, kStepCount);
    for (int i = 0; i < kStepCount && s.steps[i]; ++i)
      c.steps[pattern_][i] = s.steps[i] == '1';
    channels_.push_back(c);
  }
  savePersisted();
}

void ChannelRack::setPattern(int pattern) {
  const int p = qBound(0, pattern, kPatternCount - 1);
  if (p == pattern_) return;
  pattern_ = p;
  for (int i = 0; i < patternButtons_.size(); ++i)
    patternButtons_[i]->setChecked(i == p);
  rebuildTable();
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::setSwing(int percent) {
  swing_ = qBound(0, percent, kMaxSwing);
  if (swingSlider_ && swingSlider_->value() != swing_)
    swingSlider_->setValue(swing_);
  savePersisted();
}

bool ChannelRack::setStep(int row, int bank, int step, bool on) {
  if (row < 0 || row >= channels_.size())
    return false;
  if (bank < 0 || bank >= kPatternCount)
    return false;
  if (step < 0 || step >= kStepCount)
    return false;
  channels_[row].steps[bank][step] = on;
  rebuildTable();
  Q_EMIT edited();
  savePersisted();
  return true;
}

void ChannelRack::setMuted(int row, bool on) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].muted = on;
  rebuildTable();
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::setSolo(int row, bool on) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].solo = on;
  rebuildTable();
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::setVolume(int row, float v) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].volume = qBound(0.0f, v, 1.0f);
  rebuildTable();
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::addChannel(const QString& name) {
  Channel c;
  c.name = name.isEmpty() ? QStringLiteral("Channel %1").arg(channels_.size() + 1)
                          : name;
  for (int b = 0; b < kPatternCount; ++b)
    c.steps[b].fill(false, kStepCount);
  channels_.push_back(c);
  rebuildTable();
  setCurrent(channels_.size() - 1);
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::removeCurrent() {
  if (!hasSelection()) return;
  channels_.removeAt(current_);
  current_ = qMin(current_, (int)channels_.size() - 1);
  rebuildTable();
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::duplicateCurrent() {
  if (!hasSelection()) return;
  auto c = channels_[current_];
  c.name += " Copy";
  channels_.insert(current_ + 1, c);
  rebuildTable();
  setCurrent(current_ + 1);
  Q_EMIT edited();
  savePersisted();
}

void ChannelRack::rebuildTable() {
  model_->clear();
  model_->setRowCount(int(channels_.size()));
  model_->setColumnCount(stepBase_ + kStepCount + 2);
  QStringList hdr;
  hdr << "Channel" << "Mute" << "Solo";
  for (int i = 0; i < kStepCount; ++i) hdr << QString::number(i % 16);
  hdr << "Volume" << "Pan";
  model_->setHorizontalHeaderLabels(hdr);

  for (int r = 0; r < (int)channels_.size(); ++r) {
    auto flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    for (int c = 0; c < model_->columnCount(); ++c) {
      auto* item = new QStandardItem;
      item->setFlags(flags);
      item->setData(r, Qt::UserRole);
      model_->setItem(r, c, item);
    }
    // name editable
    model_->item(r, 0)->setText(channels_[r].name);
    model_->item(r, 0)->setFlags(flags | Qt::ItemIsEditable);
  }
  view_->horizontalHeader()->setDefaultSectionSize(26);
  view_->horizontalHeader()->resizeSection(0, 150);
  view_->horizontalHeader()->resizeSection(stepBase_ + kStepCount, 80);
  view_->horizontalHeader()->resizeSection(stepBase_ + kStepCount + 1, 52);
  view_->setColumnWidth(1, 34);
  view_->setColumnWidth(2, 34);
  view_->setModel(model_);
  view_->selectRow(qMax(0, current_));
}

void ChannelRack::setCurrent(int row) {
  current_ = qBound(-1, row, int(channels_.size()) - 1);
  if (current_ >= 0) view_->selectRow(current_);
}

int ChannelRack::columnOfStep(int logical) const {
  return logical + stepBase_;
}

void ChannelRack::loadPersisted() {
  QSettings s;
  s.beginGroup("ui/channelRack");
  QByteArray blob = s.value("channels").toByteArray();
  pattern_ = qBound(0, s.value("pattern").toInt(0), kPatternCount - 1);
  swing_ = qBound(0, s.value("swing").toInt(0), kMaxSwing);
  s.endGroup();
  if (blob.isEmpty()) return;
  auto doc = QJsonDocument::fromJson(blob);
  if (!doc.isArray()) return;
  auto arr = doc.array();
  for (const auto& v : arr) {
    auto o = v.toObject();
    Channel c;
    c.name = o.value("n").toString();
    c.muted = o.value("m").toBool(false);
    c.solo = o.value("s").toBool(false);
    c.volume = float(o.value("v").toDouble(0.8));
    c.pan = float(o.value("p").toDouble(0.0));
    for (int b = 0; b < kPatternCount; ++b) {
      c.steps[b].fill(false, kStepCount);
      const QString key = QStringLiteral("st%1").arg(b);
      QByteArray st =
          QByteArray::fromBase64(o.value(key).toString().toLatin1());
      for (int i = 0; i < st.size() && i < kStepCount; ++i)
        c.steps[b][i] = st[i] ? true : false;
    }
    // Legacy blob: a single "st" bank belongs to the first pattern.
    if (o.contains("st") && !o.contains("st0")) {
      QByteArray st = QByteArray::fromBase64(o.value("st").toString().toLatin1());
      for (int i = 0; i < st.size() && i < kStepCount; ++i)
        c.steps[0][i] = st[i] ? true : false;
    }
    channels_.push_back(c);
  }
}

void ChannelRack::savePersisted() {
  QJsonArray arr;
  for (auto& c : channels_) {
    QJsonObject o;
    o.insert("n", c.name);
    o.insert("m", c.muted);
    o.insert("s", c.solo);
    o.insert("v", double(c.volume));
    o.insert("p", double(c.pan));
    for (int b = 0; b < kPatternCount; ++b) {
      QByteArray st(kStepCount, 0);
      for (int i = 0; i < kStepCount; ++i) st[i] = c.steps[b][i] ? 1 : 0;
      o.insert(QStringLiteral("st%1").arg(b),
               QString::fromLatin1(st.toBase64()));
    }
    arr.append(o);
  }
  QSettings s;
  s.beginGroup("ui/channelRack");
  s.setValue("channels", QJsonDocument(arr).toJson(QJsonDocument::Compact));
  s.setValue("pattern", pattern_);
  s.setValue("swing", swing_);
  s.endGroup();
}

} // namespace misuc::ui