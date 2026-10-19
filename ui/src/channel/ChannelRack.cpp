#include "ChannelRack.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSlider>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "HintSystem.h"
#include "Theme.h"
#include <cmath>

#include "Theme.h"
#include "branding/Icons.h"

namespace misuc::ui {

namespace {


// Organizational color per channel type (master spec §26: use color as
// metadata, never the only identifier). Drums read as one family, pitched
// instruments another, etc.
QColor channelTypeColor(misuc::project::ChannelType t) {
  switch (t) {
    case misuc::project::ChannelType::Sampler:
      return QColor(0xff, 0x99, 0x66);
    case misuc::project::ChannelType::Instrument:
      return QColor(0x6b, 0xc3, 0xff);
    case misuc::project::ChannelType::Plugin:
      return QColor(0xb3, 0x7d, 0xff);
    case misuc::project::ChannelType::AudioClip:
      return QColor(0x8a, 0xe6, 0x8c);
    case misuc::project::ChannelType::Automation:
      return QColor(0xff, 0xd4, 0x6b);
    case misuc::project::ChannelType::Midi:
      return QColor(0xf0, 0x8a, 0xd0);
  }
  return theme::kTextFaint;
}

class RackDelegate : public QStyledItemDelegate {
public:
  QVector<ChannelRack::Channel>* channels;
  int stepBase;
  ChannelRack* rack;
  int dragRow_ = -1;
  int dragCol_ = -1;

  void paint(QPainter* p, const QStyleOptionViewItem& opt,
             const QModelIndex& index) const override {
    const int col = index.column();
    const int row = index.row();
    ChannelRack::Channel& ch = (*channels)[row];

    if (col >= stepBase && col < stepBase + rack->stepsPerPattern()) {
      const quint8 vel = ch.steps[rack->pattern()][col - stepBase];
      p->fillRect(opt.rect, theme::kPanelSunken);
      QRectF cell(opt.rect.x() + 1.5, opt.rect.y() + 1.5,
                  opt.rect.width() - 3, opt.rect.height() - 3);
      if (vel > 0) {
        // Accent intensity maps to velocity so louder steps read brighter.
        QColor accent(theme::kAccent);
        accent.setAlphaF(0.35 + 0.65 * (static_cast<float>(vel) / 127.f));
        p->setPen(Qt::NoPen);
        p->setBrush(accent);
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
      // Channel type: compact label + icon glyph. The visible name (col 1) is
      // separate from the stable identity, so where the type badge sits in the
      // table is pure presentation.
      const char* label = channelTypeName(ch.type);
      QColor badge = channelTypeColor(ch.type);
      p->setPen(Qt::NoPen);
      p->setBrush(QColor(badge.red(), badge.green(), badge.blue(), 40));
      p->drawRoundedRect(QRectF(opt.rect.x()+1, opt.rect.y()+3,
                                opt.rect.width()-2, opt.rect.height()-6), 4, 4);
      p->setPen(badge);
      p->drawText(opt.rect.adjusted(6,0,0,0), Qt::AlignVCenter | Qt::AlignLeft,
                  QString::fromLatin1(label));
      return;
    }
    if (col == 1) {
      p->setPen(theme::kText);
      p->drawText(opt.rect.adjusted(8,0,0,0), Qt::AlignVCenter | Qt::AlignLeft,
                  ch.name);
      return;
    }
    if (col == 2) {
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
    if (col == 3) {
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
    if (col == stepBase + rack->stepsPerPattern()) {
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
    if (col == stepBase + rack->stepsPerPattern() + 1) {
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
    const auto type = ev->type();
    if (type == QEvent::MouseMove && dragRow_ >= 0) {
      auto* me = static_cast<QMouseEvent*>(ev);
      const double frac = std::clamp(
          (me->position().x() - opt.rect.x()) / opt.rect.width(), 0.0, 1.0);
      ChannelRack::Channel& ch = (*channels)[dragRow_];
      if (dragCol_ == stepBase + rack->stepsPerPattern())
        ch.volume = static_cast<float>(frac);
      else if (dragCol_ == stepBase + rack->stepsPerPattern() + 1)
        ch.pan = static_cast<float>(frac * 2.0 - 1.0);
      model->dataChanged(index, index);
      rack->syncToSession();
      Q_EMIT rack->edited();
      return true;
    }
    if (type == QEvent::MouseButtonRelease && dragRow_ >= 0) {
      dragRow_ = -1;
      dragCol_ = -1;
      return true;
    }
    if (type != QEvent::MouseButtonPress &&
        type != QEvent::MouseButtonDblClick)
      return QStyledItemDelegate::editorEvent(ev, model, opt, index);
    if (index.column() >= stepBase && index.column() < stepBase + rack->stepsPerPattern()) {
      ChannelRack::Channel& ch = (*channels)[index.row()];
      QVector<quint8>& bank = ch.steps[rack->pattern()];
      const int sIdx = index.column() - stepBase;
      // Toggle: off -> on at default velocity; on -> off. (Ctrl+wheel tunes
      // velocity instead; handled in the view's event filter.)
      bank[sIdx] = bank[sIdx] == 0 ? ChannelRack::kDefaultStepVelocity : 0;
      model->dataChanged(index, index);
      rack->syncToSession();
      Q_EMIT rack->edited();
      return true;
    }
    if (index.column() == stepBase + rack->stepsPerPattern()) {
      // Drag volume: position maps to 0..1 across the cell width.
      auto* me = static_cast<QMouseEvent*>(ev);
      dragRow_ = index.row();
      dragCol_ = index.column();
      ChannelRack::Channel& ch = (*channels)[index.row()];
      const double frac = std::clamp(
          (me->position().x() - opt.rect.x()) / opt.rect.width(), 0.0, 1.0);
      ch.volume = static_cast<float>(frac);
      model->dataChanged(index, index);
      rack->syncToSession();
      Q_EMIT rack->edited();
      return true;
    }
    if (index.column() == stepBase + rack->stepsPerPattern() + 1) {
      // Drag pan: cell center = center, left/right edges = L100/R100.
      auto* me = static_cast<QMouseEvent*>(ev);
      dragRow_ = index.row();
      dragCol_ = index.column();
      ChannelRack::Channel& ch = (*channels)[index.row()];
      const double frac = std::clamp(
          (me->position().x() - opt.rect.x()) / opt.rect.width(), 0.0, 1.0);
      ch.pan = static_cast<float>(frac * 2.0 - 1.0);
      model->dataChanged(index, index);
      rack->syncToSession();
      Q_EMIT rack->edited();
      return true;
    }
    if (index.column() == 2) {
      ChannelRack::Channel& ch = (*channels)[index.row()];
      ch.muted = !ch.muted;
      model->dataChanged(index, index);
      rack->syncToSession();
      Q_EMIT rack->edited();
      return true;
    }
    if (index.column() == 3) {
      ChannelRack::Channel& ch = (*channels)[index.row()];
      ch.solo = !ch.solo;
      model->dataChanged(index, index);
      rack->syncToSession();
      Q_EMIT rack->edited();
      return true;
    }
    return QStyledItemDelegate::editorEvent(ev, model, opt, index);
  }
};

} // namespace

ChannelRack::ChannelRack(misuc::project::Session& session, QWidget* parent)
    : QWidget(parent), s_(session) {
  setObjectName(QStringLiteral("channelRack"));
  // Resizable panel: the chrome bar's content defines the practical floor.
  // Overriding it with a fixed value lets the header row shrink below its own
  // controls (Qt then compresses the box spacing into overlaps); let the
  // layout supply the width minimum and only guard the height.
  setMinimumHeight(200);
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
  cl->setContentsMargins(8, 2, 8, 2);
  cl->setSpacing(2);

  auto mkChromeBtn = [&](icons::IconId id, const QString& tip) {
    auto* b = new QToolButton(chrome);
    b->setIcon(icons::make(id, theme::kTextDim, 16));
    b->setIconSize(QSize(16, 16));
    b->setAutoRaise(true);
    b->setFixedSize(20, 22);
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);
    b->setToolTip(tip);
    chromeButtons_.push_back(b);
    return b;
  };

  auto* title = new QLabel(QStringLiteral("CHANNEL RACK"), chrome);
  title->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; "
                                      "font-weight: 800; letter-spacing: 1px;")
                           .arg(theme::kTextMuted.name()));
  cl->addWidget(title);

  // ── pattern selector: four 16-step banks, icon-first numbered toggles.
  for (int i = 0; i < kPatternCount; ++i) {
    auto* b = new QToolButton(chrome);
    b->setText(QString::number(i + 1));
    b->setCheckable(true);
    b->setChecked(i == 0);
    b->setAutoRaise(true);
    b->setFixedSize(20, 22);
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

  cl->addSpacing(3);

  // ── swing control (0..66%): groove amount for the active pattern bank.
  swingSlider_ = new QSlider(Qt::Horizontal, chrome);
  swingSlider_->setRange(0, kMaxSwing);
  swingSlider_->setSingleStep(1);
  swingSlider_->setFixedWidth(64);
  swingSlider_->setFixedHeight(22);
  swingSlider_->setFocusPolicy(Qt::NoFocus);
  swingSlider_->setCursor(Qt::PointingHandCursor);
  connect(swingSlider_, &QSlider::valueChanged, this,
          [this](int v) { setSwing(v); });
  cl->addWidget(swingSlider_);

  // ── pattern length (steps): 4..128. The rack is not hardcoded to 16 steps
  // (master spec §9); the lane resizes live and the count persists.
  auto* stepsLabel = new QLabel(QStringLiteral("Steps"), chrome);
  stepsLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 9px; "
                                           "font-weight: 700;")
                                .arg(theme::kTextMuted.name()));
  stepsLabel->setContentsMargins(2, 0, 2, 0);
  cl->addWidget(stepsLabel);
  stepsCombo_ = new QComboBox(chrome);
  stepsCombo_->setObjectName(QStringLiteral("stepsCombo"));
  stepsCombo_->setFixedWidth(52);
  stepsCombo_->setMinimumContentsLength(0);
  const int kPresets[] = {4, 8, 16, 32, 64, 128};
  for (int p : kPresets) {
    stepsCombo_->addItem(QStringLiteral("%1 steps").arg(p), p);
    if (p == stepsPerPattern_) stepsCombo_->setCurrentIndex(stepsCombo_->count() - 1);
  }
  stepsCombo_->setCursor(Qt::PointingHandCursor);
  stepsCombo_->setFocusPolicy(Qt::NoFocus);
  stepsCombo_->setStyleSheet(
      QStringLiteral("QComboBox { color: %1; background: %2; border: 1px solid %3; "
                     "border-radius: 5px; padding: 2px 6px; font-size: 10px; }")
          .arg(theme::kText.name(), theme::kChromeRaised.name(QColor::HexArgb),
               theme::kChromeBorder.name()));
  connect(stepsCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
          [this](int idx) {
            if (idx < 0) return;
            setStepsPerPattern(stepsCombo_->itemData(idx).toInt());
          });
  cl->addWidget(stepsCombo_);

  cl->addStretch(1);

  auto* prevBtn = mkChromeBtn(icons::IconId::Preview, "Preview sound (row)");
  auto* addBtn = mkChromeBtn(icons::IconId::Add, "Add channel");
  auto* dupBtn = mkChromeBtn(icons::IconId::Duplicate, "Duplicate channel");
  auto* delBtn = mkChromeBtn(icons::IconId::Remove, "Remove channel");
  auto* resetBtn = mkChromeBtn(icons::IconId::Undo, "Reset to starter kit");
  cl->addWidget(prevBtn);
  cl->addSpacing(2);
  cl->addWidget(addBtn);
  cl->addWidget(dupBtn);
  cl->addWidget(delBtn);
  cl->addSpacing(2);
  cl->addWidget(resetBtn);
  cl->addSpacing(2);
  placePatternBtn_ = new QToolButton(chrome);
  placePatternBtn_->setObjectName(QStringLiteral("placePattern"));
  placePatternBtn_->setToolTip(QStringLiteral(
      "Place Pattern: turn this bank's steps into a real MIDI region in the "
      "arrangement (editable in piano roll)"));
  placePatternBtn_->setIcon(icons::make(icons::IconId::Patterns, theme::kTextDim, 16));
  placePatternBtn_->setAutoRaise(true);
  placePatternBtn_->setFixedSize(20, 22);
  placePatternBtn_->setCursor(Qt::PointingHandCursor);
  placePatternBtn_->setFocusPolicy(Qt::NoFocus);
  connect(placePatternBtn_, &QToolButton::clicked, this,
          &ChannelRack::placePattern);
  cl->addWidget(placePatternBtn_);
  cl->addSpacing(2);
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
  view_->setObjectName(QStringLiteral("rackTable"));
  view_->setSelectionBehavior(QAbstractItemView::SelectItems);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::DoubleClicked);
  view_->setFocusPolicy(Qt::StrongFocus);
  view_->horizontalHeader()->setStretchLastSection(true);
  view_->verticalHeader()->setVisible(false);
  view_->horizontalHeader()->setVisible(true);
  view_->horizontalHeader()->setDefaultSectionSize(26);
  // The rack is a drop target for Browser audio assets (drag→Channel Rack).
  view_->setAcceptDrops(true);
  view_->viewport()->setAcceptDrops(true);
  lay->addWidget(view_);

  model_ = new QStandardItemModel(this);
  view_->setModel(model_);
  connect(model_, &QStandardItemModel::itemChanged, this,
          [this](QStandardItem* item) {
            if (!item || item->column() != 1) return;
            const int r = item->row();
            if (r < 0 || r >= channels_.size()) return;
            const QString name = item->text().trimmed();
            if (name.isEmpty() || name == channels_[r].name) return;
            channels_[r].name = name;
            syncToSession();
            Q_EMIT edited();
          });

  auto* del = new RackDelegate;
  del->channels = &channels_;
  del->stepBase = stepBase_;
  del->rack = this;
  view_->setItemDelegate(del);
  (void)del;

  // Full mouse tracking so Ctrl+wheel over a step reports the hovered cell.
  view_->viewport()->setMouseTracking(true);
  view_->viewport()->installEventFilter(this);
  view_->installEventFilter(this);

  syncFromSession();
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
  return misuc::project::rackRowFreq(row);
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
  if (stepsCombo_)
    hints->registerTool(stepsCombo_, QStringLiteral("Steps"),
                        QStringLiteral("Pattern length in steps (4–128); "
                                       "narrowing hides trailing gates"),
                        QString(), icons::IconId::Patterns,
                        [this] {
                          return QStringLiteral("%1 steps").arg(stepsPerPattern_);
                        });
  if (placePatternBtn_)
    hints->registerHint(placePatternBtn_,
                        Hint{QStringLiteral("Place Pattern"),
                             QStringLiteral("Turn this bank's steps into a real "
                                            "MIDI region in the arrangement"),
                             QString(),
                             icons::make(icons::IconId::Patterns, theme::kAccent, 16),
                             QString(), nullptr});

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
      // Channel type badge. The tip shows the type name and the stable
      // internal id, making the identity/name split visible.
      h.name = channelTypeName(ch.type);
      h.description = QStringLiteral("Channel type · id %1").arg(
          ch.id.isEmpty() ? QStringLiteral("?") : ch.id);
      return h;
    }
    if (col == 1) {
      h.name = name;
      h.description = QStringLiteral("Step-sequencer channel · %1").arg(
          ch.muted ? QStringLiteral("muted") : QStringLiteral("live"));
      if (ch.muted) h.name += QStringLiteral(" (M)");
      return h;
    }
    if (col == 2) {
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
    if (col == 3) {
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
    if (col >= self->stepBase_ && col < self->stepBase_ + self->stepsPerPattern_) {
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
    if (col == self->stepBase_ + self->stepsPerPattern_) {
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
    if (col == self->stepBase_ + self->stepsPerPattern_ + 1) {
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
  struct Seed { const char* n; misuc::project::ChannelType t;
                float v; float pan; const char* steps; };
  const Seed seeds[] = {
    {"Kick",    misuc::project::ChannelType::Sampler,    0.90f, 0.0f, "1000100010001000"},
    {"Snare",   misuc::project::ChannelType::Sampler,    0.85f, 0.0f, "0000100000001000"},
    {"Hi-Hat",  misuc::project::ChannelType::Sampler,    0.55f, 0.0f, "1010101010101010"},
    {"Bass",    misuc::project::ChannelType::Instrument, 0.80f, 0.0f, "1000001000000000"},
    {"Sampler", misuc::project::ChannelType::Sampler,    0.70f, 0.0f, "1000000010000000"},
  };
  for (auto& s : seeds) {
    Channel c;
    c.id = QString::fromStdString(s_.newChannelId());
    c.name = QString::fromLatin1(s.n);
    c.type = s.t;
    c.volume = s.v;
    c.pan = s.pan;
    for (int b = 0; b < kPatternCount; ++b)
      c.steps[b].fill(0, stepsPerPattern_);
    for (int i = 0; i < stepsPerPattern_ && s.steps[i]; ++i)
      c.steps[pattern_][i] =
          s.steps[i] == '1' ? kDefaultStepVelocity : 0;
    channels_.push_back(c);
  }
  syncToSession();
}

void ChannelRack::refresh() {
  syncFromSession();
  rebuildTable();
  if (swingSlider_) swingSlider_->setValue(swing_);
  for (int i = 0; i < patternButtons_.size(); ++i)
    patternButtons_[i]->setChecked(i == pattern_);
  if (stepsCombo_) {
    for (int i = 0; i < stepsCombo_->count(); ++i)
      if (stepsCombo_->itemData(i).toInt() == stepsPerPattern_) {
        stepsCombo_->setCurrentIndex(i);
        break;
      }
  }
}

void ChannelRack::setPattern(int pattern) {
  const int p = qBound(0, pattern, kPatternCount - 1);
  if (p == pattern_) return;
  pattern_ = p;
  for (int i = 0; i < patternButtons_.size(); ++i)
    patternButtons_[i]->setChecked(i == p);
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::setSwing(int percent) {
  swing_ = qBound(0, percent, kMaxSwing);
  if (swingSlider_ && swingSlider_->value() != swing_)
    swingSlider_->setValue(swing_);
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::setStepsPerPattern(int steps) {
  const int n = qBound(4, steps, misuc::project::kMaxStepCount);
  if (n == stepsPerPattern_)
    return;
  stepsPerPattern_ = n;
  // Resize every lane (QVector::resize pads growth with 0/off and truncates
  // shrink, exactly the off/keep-head semantics the lanes want).
  for (Channel& c : channels_)
    for (int b = 0; b < kPatternCount; ++b)
      c.steps[b].resize(n);
  // Widening the lane must not resurrect stale high steps in the engine
  // backing arrays (a later short-then-long edit would replay ghosts).
  auto& cr = s_.channelRack();
  for (auto& sc : cr.channels)
    for (int b = 0; b < kPatternCount; ++b)
      for (int i = n; i < misuc::project::kMaxStepCount; ++i)
        sc.steps[b][i] = 0;
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

bool ChannelRack::setStep(int row, int bank, int step, bool on) {
  if (row < 0 || row >= channels_.size())
    return false;
  if (bank < 0 || bank >= kPatternCount)
    return false;
  if (step < 0 || step >= stepsPerPattern_)
    return false;
  channels_[row].steps[bank][step] = on ? kDefaultStepVelocity : 0;
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
  return true;
}

// CT2010 velocity slider helper: the public slot keeps the bool-gate
// contract (on = 100) so any caller that only thinks in gates still works,
// while the wheel tuning path writes raw velocities above.
void ChannelRack::setStepVelocity(int row, int bank, int step, quint8 vel) {
  if (row < 0 || row >= channels_.size())
    return;
  if (bank < 0 || bank >= kPatternCount)
    return;
  if (step < 0 || step >= stepsPerPattern_)
    return;
  channels_[row].steps[bank][step] = qBound<quint8>(0, vel, 127);
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

quint8 ChannelRack::stepVelocity(int row, int bank, int step) const {
  if (row < 0 || row >= channels_.size())
    return 0;
  if (bank < 0 || bank >= kPatternCount)
    return 0;
  if (step < 0 || step >= stepsPerPattern_)
    return 0;
  return channels_[row].steps[bank][step];
}

void ChannelRack::setMuted(int row, bool on) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].muted = on;
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::setSolo(int row, bool on) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].solo = on;
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::setVolume(int row, float v) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].volume = qBound(0.0f, v, 1.0f);
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::setPan(int row, float p) {
  if (row < 0 || row >= channels_.size())
    return;
  channels_[row].pan = qBound(-1.0f, p, 1.0f);
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::setChannelType(int row, misuc::project::ChannelType type) {
  if (row < 0 || row >= channels_.size() || channels_[row].type == type)
    return;
  channels_[row].type = type;
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

misuc::project::ChannelType ChannelRack::channelType(int row) const {
  if (row < 0 || row >= channels_.size())
    return misuc::project::ChannelType::Sampler;
  return channels_[row].type;
}

QString ChannelRack::channelId(int row) const {
  if (row < 0 || row >= channels_.size())
    return QString();
  return channels_[row].id;
}

void ChannelRack::addChannel(const QString& name) {
  Channel c;
  c.id = QString::fromStdString(s_.newChannelId());
  c.name = name.isEmpty() ? QStringLiteral("Channel %1").arg(channels_.size() + 1)
                          : name;
  for (int b = 0; b < kPatternCount; ++b)
    c.steps[b].fill(false, stepsPerPattern_);
  channels_.push_back(c);
  rebuildTable();
  setCurrent(channels_.size() - 1);
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::addSamplerChannelFromSound(const QString& path) {
  const QFileInfo fi(path);
  if (!fi.exists() || !fi.isReadable())
    return;
  Channel c;
  c.id = QString::fromStdString(s_.newChannelId());
  const QString stem = fi.completeBaseName().isEmpty()
                           ? fi.fileName()
                           : fi.completeBaseName();
  c.name = stem;
  c.type = misuc::project::ChannelType::Sampler;
  c.soundFile = fi.absoluteFilePath();
  c.steps[pattern_].fill(false, stepsPerPattern_);
  for (int b = 0; b < kPatternCount; ++b)
    if (b != pattern_)
      c.steps[b].fill(false, stepsPerPattern_);
  channels_.push_back(c);
  rebuildTable();
  setCurrent(channels_.size() - 1);
  syncToSession();
  Q_EMIT edited();
}

QString ChannelRack::channelSoundFile(int row) const {
  if (row < 0 || row >= channels_.size())
    return QString();
  return channels_[row].soundFile;
}

void ChannelRack::removeCurrent() {
  if (!hasSelection()) return;
  channels_.removeAt(current_);
  current_ = qMin(current_, (int)channels_.size() - 1);
  rebuildTable();
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::duplicateCurrent() {
  if (!hasSelection()) return;
  auto c = channels_[current_];
  c.id = QString::fromStdString(s_.newChannelId()); // identity is never copied
  c.name += " Copy";
  channels_.insert(current_ + 1, c);
  rebuildTable();
  setCurrent(current_ + 1);
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::moveUp() {
  if (!hasSelection() || current_ <= 0) return;
  channels_.swapItemsAt(current_, current_ - 1);
  rebuildTable();
  setCurrent(current_ - 1);
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::moveDown() {
  if (!hasSelection() || current_ >= channels_.size() - 1) return;
  channels_.swapItemsAt(current_, current_ + 1);
  rebuildTable();
  setCurrent(current_ + 1);
  syncToSession();
  Q_EMIT edited();
}

void ChannelRack::rebuildTable() {
  model_->clear();
  model_->setRowCount(int(channels_.size()));
  model_->setColumnCount(stepBase_ + stepsPerPattern_ + 2);
  QStringList hdr;
  hdr << "Type" << "Channel" << "Mute" << "Solo";
  // Step header repeats the 1..16 bar count across longer lanes so a 64-step
  // pattern reads as four 16-step bars.
  for (int i = 0; i < stepsPerPattern_; ++i)
    hdr << QString::number((i % 16) + 1);
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
    model_->item(r, 1)->setText(channels_[r].name);
    model_->item(r, 1)->setFlags(flags | Qt::ItemIsEditable);
    if (!channels_[r].soundFile.isEmpty())
      model_->item(r, 1)->setToolTip(
          QStringLiteral("%1\nSample: %2")
              .arg(channels_[r].name, channels_[r].soundFile));
  }
  view_->horizontalHeader()->setDefaultSectionSize(26);
  view_->horizontalHeader()->resizeSection(0, 92);
  view_->horizontalHeader()->resizeSection(1, 150);
  view_->horizontalHeader()->resizeSection(stepBase_ + stepsPerPattern_, 80);
  view_->horizontalHeader()->resizeSection(stepBase_ + stepsPerPattern_ + 1, 52);
  view_->setColumnWidth(2, 34);
  view_->setColumnWidth(3, 34);
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

void ChannelRack::syncFromSession() {
  channels_.clear();
  const auto& cr = s_.channelRack();
  for (const misuc::project::StepChannel& sc : cr.channels) {
    Channel c;
    c.id = QString::fromStdString(sc.id);
    c.name = QString::fromStdString(sc.name);
    c.type = sc.type;
    c.muted = sc.muted;
    c.solo = sc.solo;
    c.volume = qBound(0.0f, sc.volume, 1.0f);
    c.pan = qBound(-1.0f, sc.pan, 1.0f);
    c.soundFile = QString::fromStdString(sc.soundFile);
    for (int b = 0; b < kPatternCount; ++b) {
      c.steps[b].resize(stepsPerPattern_);
      c.steps[b].fill(0);
      for (int i = 0; i < stepsPerPattern_; ++i)
        c.steps[b][i] = sc.steps[b][i] == 0 ? 0
                        : sc.steps[b][i] == 1 ? ChannelRack::kDefaultStepVelocity
                                              : sc.steps[b][i];
    }
    channels_.push_back(c);
  }
  pattern_ = qBound(0, cr.activePattern, kPatternCount - 1);
  swing_ = qBound(0, cr.swing, kMaxSwing);
  stepsPerPattern_ = std::clamp(cr.stepsPerPattern, 4, misuc::project::kMaxStepCount);
}

void ChannelRack::syncToSession() {
  auto& cr = s_.channelRack();
  cr.channels.clear();
  cr.channels.reserve(static_cast<size_t>(channels_.size()));
  for (const Channel& c : channels_) {
    misuc::project::StepChannel sc;
    sc.id = c.id.toStdString();
    sc.name = c.name.toStdString();
    sc.type = c.type;
    sc.muted = c.muted;
    sc.solo = c.solo;
    sc.volume = c.volume;
    sc.pan = c.pan;
    sc.soundFile = c.soundFile.toStdString();
    for (int b = 0; b < kPatternCount; ++b)
      for (int i = 0; i < stepsPerPattern_; ++i)
        sc.steps[b][i] = i < c.steps[b].size() ? c.steps[b][i] : 0;
    cr.channels.push_back(std::move(sc));
  }
  cr.activePattern = pattern_;
  cr.swing = swing_;
  cr.stepsPerPattern = stepsPerPattern_;
}

bool ChannelRack::eventFilter(QObject* watched, QEvent* event) {
  const bool onView = watched == view_ || watched == view_->viewport();
  if (onView && event->type() == QEvent::DragEnter) {
    auto* de = static_cast<QDragEnterEvent*>(event);
    for (const QUrl& url : de->mimeData()->urls()) {
      if (!url.isLocalFile())
        continue;
      const QString suf = QFileInfo(url.toLocalFile()).suffix().toLower();
      if (suf == QLatin1String("wav") || suf == QLatin1String("flac") ||
          suf == QLatin1String("ogg") || suf == QLatin1String("mp3") ||
          suf == QLatin1String("aif") || suf == QLatin1String("aiff")) {
        de->acceptProposedAction();
        return true;
      }
    }
    return true; // a data drag over the rack is handled (rejected) here
  }
  if (onView && event->type() == QEvent::Drop) {
    auto* drop = static_cast<QDropEvent*>(event);
    int created = 0;
    for (const QUrl& url : drop->mimeData()->urls()) {
      if (!url.isLocalFile())
        continue;
      const QString p = url.toLocalFile();
      const QString suf = QFileInfo(p).suffix().toLower();
      if (suf != QLatin1String("wav") && suf != QLatin1String("flac") &&
          suf != QLatin1String("ogg") && suf != QLatin1String("mp3") &&
          suf != QLatin1String("aif") && suf != QLatin1String("aiff"))
        continue;
      addSamplerChannelFromSound(p);
      ++created;
    }
    if (created > 0)
      drop->acceptProposedAction();
    return true;
  }
  if (onView && event->type() == QEvent::KeyPress) {
    auto* ke = static_cast<QKeyEvent*>(event);
    const Qt::KeyboardModifiers mods = ke->modifiers();
    const bool reorder = mods.testFlag(Qt::AltModifier);
    if (ke->key() == Qt::Key_Delete || ke->key() == Qt::Key_Backspace) {
      removeCurrent();
      return true;
    }
    if (reorder && ke->key() == Qt::Key_Up) {
      moveUp();
      return true;
    }
    if (reorder && ke->key() == Qt::Key_Down) {
      moveDown();
      return true;
    }
  }
  if (onView && event->type() == QEvent::Wheel) {
    auto* we = static_cast<QWheelEvent*>(event);
    // Ctrl+wheel over a step cell tunes that step's velocity (and turns the
    // step on if it was off). Plain wheel still scrolls the rack vertically.
    if (we->modifiers().testFlag(Qt::ControlModifier)) {
      const QModelIndex idx = view_->indexAt(we->position().toPoint());
      if (idx.isValid() && idx.column() >= stepBase_ &&
          idx.column() < stepBase_ + stepsPerPattern_) {
        ChannelRack::Channel& ch = channels_[idx.row()];
        QVector<quint8>& bank = ch.steps[pattern_];
        const int sIdx = idx.column() - stepBase_;
        const int delta = we->angleDelta().y();
        if (delta != 0) {
          const int dir = delta > 0 ? 1 : -1;
          int v = bank[sIdx];
          if (v == 0) v = kDefaultStepVelocity;
          v = qBound(1, v + dir * 8, 127);
          bank[sIdx] = static_cast<quint8>(v);
          model_->dataChanged(idx, idx);
          syncToSession();
          Q_EMIT edited();
          return true;
        }
      }
      // Ctrl+wheel elsewhere in the rack: horizontal scroll handled by the
      // view; fall through so the view scrolls normally.
    }
  }
  return QWidget::eventFilter(watched, event);
}

} // namespace misuc::ui