#include "HintSystem.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "Theme.h"

namespace misuc::ui {

namespace {
constexpr int kChipH = 18;
constexpr int kPad = 10;
constexpr int kChipGap = 8;
} // namespace

HintBar::HintBar(QWidget* parent) : QLabel(parent) {
  setObjectName(QStringLiteral("hintBar"));
  setMinimumHeight(26);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setAttribute(Qt::WA_Hover);
}

void HintBar::showHint(const Hint& hint) {
  hint_ = hint;
  update();
}

void HintBar::clearHint() {
  hint_ = Hint{};
  update();
}

void HintBar::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setFont(theme::uiFont());

  const int yc = height() / 2;

  // Shortcut chip on the right.
  int chipW = 0;
  if (!hint_.shortcut.isEmpty()) {
    QFont chipFont = theme::monoFont(9);
    chipFont.setBold(true);
    QFontMetrics fm(chipFont);
    chipW = fm.horizontalAdvance(hint_.shortcut) + 16;
    QRectF chip(width() - chipW - 6, yc - kChipH / 2, chipW, kChipH);
    QPainterPath path;
    path.addRoundedRect(chip, 5, 5);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kAccentSoft);
    p.drawPath(path);
    p.setPen(theme::kAccent);
    p.setFont(chipFont);
    p.drawText(chip, Qt::AlignCenter, hint_.shortcut);
  }

  // Available draw width.
  const int avail = width() - kPad - (chipW ? chipW + kChipGap : 0);
  int x = kPad;

  if (!hint_.name.isEmpty()) {
    QFont nameFont = theme::uiFont(10, QFont::DemiBold);
    p.setFont(nameFont);
    p.setPen(theme::kAccent);
    const QString name = QFontMetrics(nameFont).elidedText(
        hint_.name, Qt::ElideRight, avail);
    p.drawText(QRect(x, 0, name.size(), height()), Qt::AlignVCenter | Qt::AlignLeft,
               name);
    x += QFontMetrics(nameFont).horizontalAdvance(name) + 10;
  }
  if (!hint_.description.isEmpty()) {
    p.setFont(theme::uiFont());
    p.setPen(theme::kTextDim);
    const int descAvail = avail - (x - kPad);
    const QString desc = QFontMetrics(theme::uiFont()).elidedText(
        hint_.description, Qt::ElideRight, qMax(descAvail, 40));
    p.drawText(QRect(x, 0, descAvail, height()), Qt::AlignVCenter | Qt::AlignLeft,
               desc);
  } else if (hint_.value) {
    p.setFont(theme::uiFont());
    p.setPen(theme::kTextDim);
    const int descAvail = avail - (x - kPad);
    p.drawText(QRect(x, 0, descAvail, height()), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("(hold for details)"));
  }

  // Live value on the right of the text run, before the chip.
  if (hint_.value) {
    QString v;
    try {
      const QString s = hint_.value();
      v = s.isEmpty() ? QString() : s;
    } catch (...) {
      v.clear();
    }
    if (!v.isEmpty()) {
      const QFont vf = theme::monoFont(10);
      QFontMetrics vm(vf);
      const int vw = qMin(vm.horizontalAdvance(v) + 22, 170);
      p.setFont(vf);
      p.setPen(theme::kTextSecondary);
      p.drawText(QRect(x + 8, 0, vw, height()), Qt::AlignVCenter | Qt::AlignLeft,
                 v);
    }
  }
}

HintManager::HintManager(HintBar* bar, QWidget* parent)
    : QObject(parent), bar_(bar) {
  if (bar_) {
    setDefaultHint(Hint{QStringLiteral("Misuc"),
                        QStringLiteral("Hover any control to learn it"),
                        QString(), nullptr});
  }
}

void HintManager::setDefaultHint(Hint hint) {
  defaultHint_ = std::move(hint);
  if (!hovered_)
    bar_->showHint(defaultHint_);
}

void HintManager::registerHint(QWidget* widget, Hint hint) {
  if (!widget)
    return;
  hints_[widget] = std::move(hint);
  widget->installEventFilter(this);
}

void HintManager::registerHint(QWidget* widget, const QString& name,
                               const QString& description,
                               const QString& shortcut,
                               std::function<QString()> value) {
  registerHint(widget, Hint{name, description, shortcut, std::move(value)});
}

void HintManager::refresh() {
  if (hovered_) {
    auto it = hints_.find(hovered_);
    if (it != hints_.end()) {
      bar_->showHint(it->second);
      return;
    }
  }
  if (defaultHint_.value) {
    // Refresh live default value (e.g. transport position).
    Hint h = defaultHint_;
    bar_->showHint(h);
  }
}

bool HintManager::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
    auto widget = qobject_cast<QWidget*>(watched);
    if (widget) {
      auto it = hints_.find(widget);
      if (it != hints_.end()) {
        hovered_ = widget;
        bar_->showHint(it->second);
      }
    }
  } else if (event->type() == QEvent::Leave ||
             event->type() == QEvent::HoverLeave) {
    if (watched == hovered_) {
      hovered_ = nullptr;
      bar_->showHint(defaultHint_);
    }
  }
  return QObject::eventFilter(watched, event);
}

} // namespace misuc::ui