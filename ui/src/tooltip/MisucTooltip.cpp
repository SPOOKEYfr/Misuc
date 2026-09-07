#include "MisucTooltip.h"

#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include "Theme.h"
#include "branding/Icons.h"
#include "commands/MisucCommandRegistry.h"

namespace misuc::ui {

namespace {
constexpr int kDelayMs = 380;
constexpr int kFollowMs = 30;
constexpr int kRadius = 8;
} // namespace

MisucTooltip& MisucTooltip::instance() {
  static MisucTooltip inst;
  return inst;
}

MisucTooltip::MisucTooltip(QWidget* parent) : QWidget(parent) {
  setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                 Qt::WindowDoesNotAcceptFocus);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setAttribute(Qt::WA_TranslucentBackground);
  setMouseTracking(true);

  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(12, 8, 12, 8);
  lay->setSpacing(2);

  title_ = new QLabel(this);
  title_->setWordWrap(false);
  lay->addWidget(title_);

  body_ = new QLabel(this);
  body_->setWordWrap(false);
  lay->addWidget(body_);

  meta_ = new QLabel(this);
  meta_->setWordWrap(false);
  lay->addWidget(meta_);

  // Delayed pop + gentle cursor-follow so the tooltip never feels jumpy.
  delay_ = new QTimer(this);
  delay_->setSingleShot(true);
  delay_->setInterval(kDelayMs);
  connect(delay_, &QTimer::timeout, this, &MisucTooltip::showNow);

  mover_ = new QTimer(this);
  mover_->setInterval(kFollowMs);
  connect(mover_, &QTimer::timeout, this, &MisucTooltip::followCursor);
}

void MisucTooltip::attach(QWidget* target, const QString& commandId) {
  if (!target)
    return;
  MisucTooltip& tip = instance();
  target->installEventFilter(&tip);
  // Remember the newest command for this widget; the tooltip resolves it on
  // hover.
  target->setProperty("misucTooltipCommand", commandId);
}

void MisucTooltip::build(const QString& commandId) {
  const core::MisucCommandRegistry& reg = core::MisucCommandRegistry::instance();
  const core::MisucCommand* cmd = reg.command(commandId);
  if (cmd == nullptr) {
    commandId_.clear();
    return;
  }
  commandId_ = commandId;
  QFont titleFont = theme::uiFont(10, QFont::DemiBold);
  title_->setFont(titleFont);
  title_->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                            .arg(theme::kAccent.name()));
  title_->setText(cmd->name);

  QFont bodyFont = theme::uiFont(9);
  body_->setFont(bodyFont);
  body_->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                           .arg(theme::kTextDim.name()));
  body_->setText(cmd->description);

  QString meta = cmd->category;
  if (!cmd->shortcut.isEmpty()) {
    if (!meta.isEmpty())
      meta += QStringLiteral(" • ");
    meta += cmd->shortcut.toString(QKeySequence::NativeText);
  }
  QFont metaFont = theme::monoFont(8);
  meta_->setFont(metaFont);
  meta_->setStyleSheet(QStringLiteral("color:%1;background:transparent;")
                           .arg(theme::kTextFaint.name()));
  meta_->setText(meta);
  adjustSize();
}

bool MisucTooltip::eventFilter(QObject* watched, QEvent* event) {
  switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter: {
      auto widget = qobject_cast<QWidget*>(watched);
      if (!widget)
        break;
      const QVariant id = widget->property("misucTooltipCommand");
      if (!id.isValid() || id.toString().isEmpty())
        break;
      build(id.toString());
      if (commandId_.isEmpty())
        break;
      hovered_ = widget;
      scheduleShow();
      break;
    }
    case QEvent::Leave:
    case QEvent::HoverLeave:
      if (watched == hovered_)
        hideNow();
      break;
    case QEvent::MouseButtonPress:
    case QEvent::KeyPress:
    case QEvent::WindowDeactivate:
      hideNow();
      break;
    default:
      break;
  }
  return QObject::eventFilter(watched, event);
}

void MisucTooltip::scheduleShow() {
  delay_->start();
}

void MisucTooltip::showNow() {
  if (commandId_.isEmpty())
    return;
  followCursor();
  show();
  raise();
  mover_->start();
}

void MisucTooltip::followCursor() {
  const QPoint pos = QCursor::pos();
  QScreen* screen = QGuiApplication::screenAt(pos);
  QRect avail = screen ? screen->availableGeometry()
                       : QGuiApplication::primaryScreen()->availableGeometry();
  QPoint p = pos + QPoint(14, 20);
  if (p.x() + width() > avail.right())
    p.setX(pos.x() - width() - 12);
  if (p.y() + height() > avail.bottom())
    p.setY(pos.y() - height() - 10);
  move(qMax(avail.left(), p.x()), qMax(avail.top(), p.y()));
}

void MisucTooltip::hideNow() {
  delay_->stop();
  mover_->stop();
  hovered_ = nullptr;
  hide();
}

void MisucTooltip::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPainterPath path;
  path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius,
                      kRadius);
  p.setPen(QPen(theme::kGlassBorder, 1));
  p.setBrush(theme::kTooltipBg);
  p.drawPath(path);
}

void MisucTooltip::hideEvent(QHideEvent* event) {
  mover_->stop();
  delay_->stop();
  QWidget::hideEvent(event);
}

} // namespace misuc::ui