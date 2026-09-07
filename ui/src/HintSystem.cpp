#include "HintSystem.h"

#include <QEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "Theme.h"
#include "commands/MisucCommandRegistry.h"

namespace misuc::ui {

namespace {
constexpr int kChipH = 18;
constexpr int kPad = 10;
constexpr int kChipGap = 8;
} // namespace

Hint hintFromCommand(const QString& commandId) {
  const core::MisucCommandRegistry& reg = core::MisucCommandRegistry::instance();
  const core::MisucCommand* cmd = reg.command(commandId);
  if (cmd == nullptr)
    return Hint{};
  Hint h;
  h.name = cmd->name;
  h.description = cmd->description;
  h.commandId = commandId;
  if (cmd->icon != icons::IconId::None)
    h.icon = icons::make(cmd->icon, theme::kAccent);
  if (!cmd->shortcut.isEmpty())
    h.shortcut = cmd->shortcut.toString(QKeySequence::NativeText);
  h.value = [commandId] {
    const core::MisucCommandRegistry& r = core::MisucCommandRegistry::instance();
    const core::MisucCommand* c = r.command(commandId);
    if (c && c->value) {
      try {
        return c->value();
      } catch (...) {
        return QString();
      }
    }
    if (c && c->checked) {
      bool on = false;
      try {
        on = c->checked();
      } catch (...) {
        on = false;
      }
      return on ? QStringLiteral("on") : QStringLiteral("off");
    }
    return QString();
  };
  return h;
}

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

  // Lead icon (registry-backed hints carry the command glyph).
  if (!hint_.icon.isNull()) {
    const QIcon ic = hint_.icon;
    const QRectF icr(x, yc - 9, 18, 18);
    p.drawPixmap(icr, ic.pixmap(18, 18), QRectF(0, 0, 18, 18));
    x += 24;
  }

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
                        QString(), QIcon(), QString(), nullptr});
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
  registerHint(widget, Hint{name, description, shortcut, QIcon(), QString(),
                            std::move(value)});
}

void HintManager::registerCommand(QWidget* widget, const QString& commandId) {
  if (!widget)
    return;
  commandIds_[widget] = commandId;
  widget->installEventFilter(this);
}

Hint HintManager::hintFor(const QWidget* widget) const {
  auto itId = commandIds_.find(const_cast<QWidget*>(widget));
  if (itId != commandIds_.end())
    return hintFromCommand(itId->second);
  auto it = hints_.find(const_cast<QWidget*>(widget));
  return it == hints_.end() ? Hint{} : it->second;
}

void HintManager::refresh() {
  if (hovered_) {
    auto itId = commandIds_.find(hovered_);
    if (itId != commandIds_.end()) {
      bar_->showHint(hintFromCommand(itId->second));
      return;
    }
    auto it = hints_.find(hovered_);
    if (it != hints_.end()) {
      bar_->showHint(it->second);
      return;
    }
  }
  if (defaultHint_.value) {
    // Refresh live default value (e.g. transport position).
    bar_->showHint(defaultHint_);
  }
}

bool HintManager::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) {
    auto widget = qobject_cast<QWidget*>(watched);
    if (widget) {
      const Hint h = hintFor(widget);
      if (!h.name.isEmpty() || !h.description.isEmpty()) {
        hovered_ = widget;
        bar_->showHint(h);
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