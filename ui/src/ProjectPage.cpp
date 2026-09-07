#include "ProjectPage.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>

#include "Theme.h"

namespace misuc::ui {

ProjectPage::ProjectPage(misuc::project::Session& session, QWidget* parent)
    : QWidget(parent), session_(&session) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 24);
  root->setSpacing(16);

  {
    auto* t = new QLabel(QStringLiteral("Project"), this);
    t->setFont(theme::uiFont(13, QFont::Bold));
    root->addWidget(t);
    auto* s = new QLabel(
        QStringLiteral(
            "Project settings are stored in the .misuc format. Use File > "
            "New / Open / Save to manage sessions."),
        this);
    s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    s->setWordWrap(true);
    root->addWidget(s);
  }

  auto* form = new QFormLayout();
  form->setHorizontalSpacing(24);
  nameEdit_ = new QLineEdit(QString::fromStdString(session_->name()), this);
  tempoEdit_ = new QLineEdit(QString::number(session_->tempo()), this);
  tempoEdit_->setToolTip(QStringLiteral("20 – 400 BPM"));
  form->addRow(QStringLiteral("Name"), nameEdit_);
  form->addRow(QStringLiteral("Tempo (BPM)"), tempoEdit_);
  root->addLayout(form);

  root->addStretch();

  connect(nameEdit_, &QLineEdit::editingFinished, this, &ProjectPage::commit);
  connect(tempoEdit_, &QLineEdit::editingFinished, this, &ProjectPage::commit);
}

void ProjectPage::refresh() {
  nameEdit_->setText(QString::fromStdString(session_->name()));
  tempoEdit_->setText(QString::number(session_->tempo()));
}

void ProjectPage::commit() {
  session_->setName(nameEdit_->text().toStdString());
  const double tempo = tempoEdit_->text().toDouble();
  if (tempo >= 20.0 && tempo <= 400.0)
    session_->setTempo(tempo);
  emit edited();
}

} // namespace misuc::ui