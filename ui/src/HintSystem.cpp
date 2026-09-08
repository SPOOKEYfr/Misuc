#include "HintSystem.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <optional>

#include "Theme.h"
#include "commands/MisucCommandRegistry.h"

namespace misuc::ui {

namespace {
// Hover shows the hint only after the cursor has settled, so flicking across a
// toolbar never strobes the status area; keyboard focus shows it immediately.
constexpr int kHoverDelayMs = 360;
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

HintBar::HintBar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("hintBar"));
  setAttribute(Qt::WA_Hover);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setMinimumHeight(30);
  setMaximumHeight(34);

  auto* lay = new QHBoxLayout(this);
  lay->setContentsMargins(12, 0, 10, 0);
  lay->setSpacing(8);

  glyph_ = new QLabel(this);
  glyph_->setObjectName(QStringLiteral("hintGlyph"));
  glyph_->setFixedSize(18, 18);
  glyph_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  glyph_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

  name_ = new QLabel(this);
  name_->setObjectName(QStringLiteral("hintName"));
  name_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  name_->setMinimumWidth(64);
  name_->setMaximumWidth(280);
  name_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  name_->setFont(theme::uiFont(10, QFont::DemiBold));

  desc_ = new QLabel(this);
  desc_->setObjectName(QStringLiteral("hintDesc"));
  desc_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  desc_->setMinimumWidth(40);
  desc_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  desc_->setFont(theme::uiFont());
  desc_->setTextFormat(Qt::PlainText);

  value_ = new QLabel(this);
  value_->setObjectName(QStringLiteral("hintValue"));
  value_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  value_->setMaximumWidth(180);
  value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  value_->setFont(theme::monoFont(10));
  value_->setVisible(false);

  chip_ = new QLabel(this);
  chip_->setObjectName(QStringLiteral("hintChip"));
  chip_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  chip_->setMinimumHeight(20);
  chip_->setAlignment(Qt::AlignCenter);
  chip_->setFont(theme::monoFont(9));
  chip_->setVisible(false);

  lay->addWidget(glyph_);
  lay->addWidget(name_);
  lay->addWidget(desc_, 1);
  lay->addWidget(value_);
  lay->addWidget(chip_);
  lay->addStretch(1);

  clearHint();
}

void HintBar::showHint(const Hint& hint) {
  hint_ = hint;

  if (!hint_.icon.isNull()) {
    glyph_->setPixmap(hint_.icon.pixmap(18, 18));
    glyph_->show();
  } else {
    glyph_->hide();
  }

  name_->setText(hint_.name);

  const bool hasDesc = !hint_.description.isEmpty();
  desc_->setText(hasDesc ? hint_.description : QString());
  desc_->setVisible(hasDesc);

  if (hint_.value) {
    QString v;
    try {
      const QString s = hint_.value();
      v = s.isEmpty() ? QString() : s;
    } catch (...) {
      v.clear();
    }
    value_->setText(v);
    value_->setVisible(!v.isEmpty());
  } else {
    value_->setText(QString());
    value_->setVisible(false);
  }

  if (!hint_.shortcut.isEmpty()) {
    chip_->setText(hint_.shortcut);
    chip_->setStyleSheet(
        QStringLiteral("background-color: %1; color: %2; border: 1px solid %3;"
                       "border-radius: 10px; padding: 1px 8px; font-weight: 700;")
            .arg(theme::kAccentSoft.name(), theme::kAccent.name(),
                 theme::kAccent.name()));
    chip_->show();
  } else {
    chip_->setText(QString());
    chip_->hide();
  }

  elideLabels();
}

void HintBar::clearHint() {
  showHint(Hint{});
}

void HintBar::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  elideLabels();
}

void HintBar::elideLabels() {
  if (!name_ || !desc_)
    return;
  // Elide against each label's current width so long purpose text never
  // overflows its cell or the bar (labels share the row via a QHBoxLayout).
  const QFontMetrics nameFm(name_->font());
  const int nameW = name_->width();
  if (nameW > 0)
    name_->setText(nameFm.elidedText(hint_.name, Qt::ElideRight, nameW));
  const QFontMetrics descFm(desc_->font());
  const int descW = desc_->width();
  const QString fullDesc = hint_.description;
  if (descW > 0) {
    desc_->setText(descFm.elidedText(fullDesc, Qt::ElideRight, descW));
  }
  const QFontMetrics valFm(value_->font());
  const int valW = value_->width();
  if (value_->isVisible() && valW > 0) {
    const QString fullVal = value_->text();
    value_->setText(valFm.elidedText(fullVal, Qt::ElideRight, valW));
  }
}

HintManager::HintManager(HintBar* bar, QWidget* parent)
    : QObject(parent), bar_(bar) {
  delay_ = new QTimer(this);
  delay_->setSingleShot(true);
  delay_->setInterval(kHoverDelayMs);
  connect(delay_, &QTimer::timeout, this, &HintManager::settleHover);
  if (bar_) {
    setDefaultHint(Hint{QStringLiteral("Misuc"),
                        QStringLiteral("Hover any control to learn it"),
                        QString(), QIcon(), QString(), nullptr});
  }
}

void HintManager::setDefaultHint(Hint hint) {
  defaultHint_ = std::move(hint);
  if (hovered_.isNull() && focused_.isNull())
    bar_->showHint(defaultHint_);
}

int HintManager::findEntry(const QWidget* widget) const {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].widget == widget && !entries_[i].widget.isNull())
      return static_cast<int>(i);
  return -1;
}

void HintManager::purgeEntry(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size()))
    return;
  const QWidget* w = entries_[index].widget.data();
  entries_.erase(entries_.begin() + index);
  if (hovered_ == w)
    hovered_.clear();
  if (focused_ == w)
    focused_.clear();
}

void HintManager::registerHint(QWidget* widget, Hint hint) {
  if (!widget)
    return;
  const int existing = findEntry(widget);
  if (existing >= 0) {
    entries_[existing].hint = std::move(hint);
    entries_[existing].commandId.clear();
    entries_[existing].resolve = nullptr;
    return;
  }
  entries_.push_back(Entry{QPointer<QWidget>(widget), std::move(hint),
                           QString(), nullptr});
  connect(widget, &QObject::destroyed, this, [this, w = QPointer<QWidget>(widget)] {
    if (w.isNull())
      return;
    const int idx = findEntry(w);
    if (idx >= 0)
      purgeEntry(idx);
  });
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
  const int existing = findEntry(widget);
  if (existing >= 0) {
    entries_[existing].commandId = commandId;
    entries_[existing].hint = Hint{};
    entries_[existing].resolve = nullptr;
    return;
  }
  entries_.push_back(Entry{QPointer<QWidget>(widget), Hint{}, commandId,
                           nullptr});
  connect(widget, &QObject::destroyed, this, [this, w = QPointer<QWidget>(widget)] {
    if (w.isNull())
      return;
    const int idx = findEntry(w);
    if (idx >= 0)
      purgeEntry(idx);
  });
  widget->installEventFilter(this);
}

void HintManager::registerTool(QWidget* widget, const QString& name,
                               const QString& description,
                               const QString& shortcut, icons::IconId icon,
                               std::function<QString()> value) {
  Hint h{name, description, shortcut, QIcon(), QString(), std::move(value)};
  if (icon != icons::IconId::None)
    h.icon = icons::make(icon, theme::kAccent);
  registerHint(widget, std::move(h));
  widget->setToolTip(QString());
}

void HintManager::registerCustom(
    QWidget* widget, std::function<std::optional<Hint>(const QPoint&)> resolve) {
  if (!widget)
    return;
  const int existing = findEntry(widget);
  if (existing >= 0) {
    entries_[existing].resolve = std::move(resolve);
    return;
  }
  entries_.push_back(
      Entry{QPointer<QWidget>(widget), Hint{}, QString(), std::move(resolve)});
  connect(widget, &QObject::destroyed, this, [this, w = QPointer<QWidget>(widget)] {
    if (w.isNull())
      return;
    const int idx = findEntry(w);
    if (idx >= 0)
      purgeEntry(idx);
  });
  widget->installEventFilter(this);
}

Hint HintManager::hintFor(const QWidget* widget, const QPoint& pos) const {
  const int idx = findEntry(widget);
  if (idx < 0)
    return Hint{};
  const Entry& e = entries_[idx];
  if (e.resolve) {
    try {
      const auto r = e.resolve(pos);
      if (r)
        return *r;
    } catch (...) {
      return Hint{};
    }
  }
  if (!e.commandId.isEmpty())
    return hintFromCommand(e.commandId);
  return e.hint;
}

void HintManager::refresh() {
  if (!hovered_.isNull()) {
    const int idx = findEntry(hovered_);
    if (idx >= 0) {
      bar_->showHint(hintFor(hovered_, lastPos_));
      return;
    }
  }
  if (!focused_.isNull()) {
    const int idx = findEntry(focused_);
    if (idx >= 0) {
      bar_->showHint(entries_[idx].commandId.isEmpty()
                         ? entries_[idx].hint
                         : hintFromCommand(entries_[idx].commandId));
      return;
    }
  }
  if (defaultHint_.value) {
    bar_->showHint(defaultHint_);
  }
}

bool HintManager::eventFilter(QObject* watched, QEvent* event) {
  auto widget = qobject_cast<QWidget*>(watched);
  if (!widget)
    return false;
  // A QPointer turns null the moment ~QObject begins, so if this event was
  // queued into an object currently being destroyed we bail before ever
  // touching it — this is what previously crashed on mouse-move teardown.
  const QPointer<QWidget> guard(widget);
  if (guard.isNull() || findEntry(widget) < 0)
    return false;
  switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter: {
      const QWidget* first = hovered_.data();
      hovered_ = widget;
      if (hovered_ == first && delay_->isActive())
        return false; // already settling on the same widget
      if (delay_->isActive())
        delay_->stop();
      delay_->start();
      return false;
    }
    case QEvent::MouseMove:
    case QEvent::HoverMove: {
      if (widget == hovered_) {
        const int idx = findEntry(widget);
        if (idx >= 0 && entries_[idx].resolve) {
          QPoint local;
          if (event->type() == QEvent::MouseMove)
            local = static_cast<QMouseEvent*>(event)->position().toPoint();
          else
            local = static_cast<QHoverEvent*>(event)->position().toPoint();
          lastPos_ = local;
          try {
            const auto r = entries_[idx].resolve(local);
            if (r)
              bar_->showHint(*r);
          } catch (...) {
          }
        }
      }
      return false;
    }
    case QEvent::HoverLeave:
    case QEvent::Leave:
      if (widget == hovered_) {
        hovered_.clear();
        if (delay_->isActive())
          delay_->stop();
        if (!focused_.isNull())
          showFor(focused_);
        else
          bar_->showHint(defaultHint_);
      }
      return false;
    case QEvent::FocusIn:
      if (widget != focused_) {
        focused_ = widget;
        if (delay_->isActive())
          delay_->stop();
        showFor(widget);
      }
      return false;
    case QEvent::FocusOut:
      if (widget == focused_) {
        focused_.clear();
        if (hovered_.isNull())
          bar_->showHint(defaultHint_);
      }
      return false;
    default:
      break;
  }
  return QObject::eventFilter(watched, event);
}

void HintManager::settleHover() {
  if (!hovered_.isNull())
    showFor(hovered_);
}

void HintManager::showFor(QWidget* widget) {
  const Hint h = hintFor(widget, QPoint());
  if (!h.name.isEmpty() || !h.description.isEmpty())
    bar_->showHint(h);
  else
    bar_->showHint(defaultHint_);
}

} // namespace misuc::ui