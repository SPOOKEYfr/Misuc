#include "ChannelRack.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardItemModel>
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
      bool on = ch.steps[col - stepBase];
      p->fillRect(opt.rect, theme::kPanelSunken);
      QRectF cell(opt.rect.x() + 1.5, opt.rect.y() + 1.5,
                  opt.rect.width() - 3, opt.rect.height() - 3);
      if (on) {
        p->setPen(Qt::NoPen);
        p->setBrush(theme::kAccent);
        p->drawRoundedRect(cell, 3, 3);
      } else {
        p->setBrush(theme::kGridRow);
        p->setPen(QColor(0,0,0,0));
        p->drawRoundedRect(cell, 3, 3);
      }
      if (opt.state.testFlag(QStyle::State_Selected)) {
        p->setPen(theme::kAccent);
        p->drawRect(opt.rect.adjusted(0,0,-1,-1));
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
      p->setBrush(ch.muted ? QColor(0xff,0x5c,0x5c) : theme::kGridRow);
      p->drawRoundedRect(r, 4, 4);
      p->setPen(Qt::NoPen);
      p->setBrush(ch.muted ? theme::kDanger : theme::kTextFaint);
      p->drawRect(QRectF(r.center().x()-6.5, r.center().y()-0.5, 13, 1));
      return;
    }
    if (col == 2) {
      auto r = opt.rect.adjusted(2,3,-2,-3);
      p->setPen(Qt::NoPen);
      p->setBrush(ch.solo ? QColor(0xff,0xb4,0x54) : theme::kGridRow);
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
      ch.steps[index.column() - stepBase] = !ch.steps[index.column() - stepBase];
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

  connect(view_, &QTableView::clicked, this, [this](const QModelIndex& i) {
    setCurrent(i.row());
    Q_EMIT channelSelected(i.row());
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
    c.steps.fill(false, kStepCount);
    for (int i = 0; i < kStepCount && s.steps[i]; ++i)
      c.steps[i] = s.steps[i] == '1';
    channels_.push_back(c);
  }
  savePersisted();
}

void ChannelRack::addChannel(const QString& name) {
  Channel c;
  c.name = name.isEmpty() ? QStringLiteral("Channel %1").arg(channels_.size() + 1)
                          : name;
  c.steps.fill(false, kStepCount);
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
    c.steps.fill(false, kStepCount);
    QByteArray st = QByteArray::fromBase64(o.value("st").toString().toLatin1());
    for (int i = 0; i < st.size() && i < kStepCount; ++i)
      c.steps[i] = st[i] ? true : false;
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
    QByteArray st(kStepCount, 0);
    for (int i = 0; i < kStepCount; ++i) st[i] = c.steps[i] ? 1 : 0;
    o.insert("st", QString::fromLatin1(st.toBase64()));
    arr.append(o);
  }
  QSettings s;
  s.beginGroup("ui/channelRack");
  s.setValue("channels", QJsonDocument(arr).toJson(QJsonDocument::Compact));
  s.endGroup();
}

} // namespace misuc::ui