#include "branding/AboutDialog.h"

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include "Theme.h"
#include "branding/Logo.h"
#include "theme/ThemeEngine.h"

namespace misuc::ui {

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("About Misuc"));
  setModal(true);
  buildUi();
}

void AboutDialog::buildUi() {
  const QColor accent = theme::kAccent;
  const QColor accentSoft = theme::kAccentSoft;

  setStyleSheet(QStringLiteral(
      "QDialog { background: %1; }"
      "QLabel { color: %2; border: none; }"
      "QLabel#aboutHeadTitle { font-size: 18pt; font-weight: 600; color: %3; }"
      "QLabel#aboutHeadSub { color: %4; }"
      "QLabel#aboutCaption { color: %5; }"
      "QLabel#aboutPhoto { border: 1px solid %6; border-radius: 6px; }")
                    .arg(theme::kPanel.name(), theme::kText.name(),
                         theme::kAccent.name(), theme::kTextDim.name(),
                         theme::kTextFaint.name(), theme::kBorderBright.name()));

  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(20, 20, 20, 16);
  root->setSpacing(12);

  // Brand header: vector logo tile + name + version.
  auto* head = new QHBoxLayout;
  auto* logo = new QLabel;
  logo->setPixmap(logoPixmap(72, accent, accentSoft));
  head->addWidget(logo, 0, Qt::AlignVCenter);
  auto* headBox = new QVBoxLayout;
  auto* title = new QLabel(QApplication::applicationName());
  title->setObjectName(QStringLiteral("aboutHeadTitle"));
  auto* version = new QLabel(QStringLiteral("Version %1 - a Linux DAW")
                                 .arg(QApplication::applicationVersion()));
  version->setObjectName(QStringLiteral("aboutHeadSub"));
  headBox->addWidget(title);
  headBox->addWidget(version);
  head->addLayout(headBox, 1);
  head->addStretch(1);
  root->addLayout(head);

  auto* rule = new QFrame;
  rule->setFrameShape(QFrame::HLine);
  rule->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kBorder.name()));
  root->addWidget(rule);

  // Author photo + contact, side by side.
  auto* body = new QHBoxLayout;
  body->setSpacing(16);
  auto* photo = new QLabel;
  photo->setObjectName(QStringLiteral("aboutPhoto"));
  QPixmap raw(QStringLiteral(":/about/obhup.png"));
  if (!raw.isNull())
    photo->setPixmap(raw.scaled(148, 148, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation));
  body->addWidget(photo, 0, Qt::AlignTop);

  auto* info = new QVBoxLayout;
  info->setSpacing(4);
  auto* who = new QLabel(QStringLiteral("<b>Bhupendra Nath</b>"));
  who->setObjectName(QStringLiteral("aboutCaption"));
  auto* mail = new QLabel(
      QStringLiteral("<a href=\"mailto:nathbhupi5@gmail.com\">"
                     "nathbhupi5@gmail.com</a>"));
  mail->setOpenExternalLinks(true);
  auto* note = new QLabel(QStringLiteral(
      "Author, maintainer and designer of Misuc."));
  note->setObjectName(QStringLiteral("aboutCaption"));
  info->addWidget(who);
  info->addWidget(mail);
  info->addWidget(note);
  info->addStretch(1);
  body->addLayout(info, 1);
  root->addLayout(body);

  root->addStretch(1);

  auto* btns = new QHBoxLayout;
  btns->addStretch(1);
  auto* ok = new QPushButton(QStringLiteral("Close"));
  connect(ok, &QPushButton::clicked, this, &QDialog::accept);
  btns->addWidget(ok);
  root->addLayout(btns);

  // Make mail links use the theme accent and keep the dialog at a fixed size.
  QPalette pal = palette();
  pal.setColor(QPalette::Link, accent);
  setPalette(pal);
  setFixedWidth(440);
}

} // namespace misuc::ui