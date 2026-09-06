#include "ProjectPage.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>

#include <misuc/engine/audio/AudioEngine.h>

#include "Theme.h"

namespace misuc::ui {

ProjectPage::ProjectPage(misuc::engine::AudioEngine& engine, QWidget* parent)
    : QWidget(parent), engine_(engine) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 24);
  root->setSpacing(16);

  {
    auto* t = new QLabel(QStringLiteral("Project"), this);
    t->setFont(theme::uiFont(13, QFont::Bold));
    root->addWidget(t);
    auto* s = new QLabel(
        QStringLiteral("Project settings are stored in the .misuc format."),
        this);
    s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
    root->addWidget(s);
  }

  auto* form = new QFormLayout();
  form->setHorizontalSpacing(24);
  nameEdit_ = new QLineEdit(QString::fromStdString(session_.name()), this);
  tempoEdit_ = new QLineEdit(QString::number(session_.tempo()), this);
  form->addRow(QStringLiteral("Name"), nameEdit_);
  form->addRow(QStringLiteral("Tempo (BPM)"), tempoEdit_);
  root->addLayout(form);

  auto* actions = new QHBoxLayout();
  auto* saveBtn = new QPushButton(QStringLiteral("Save .misuc…"), this);
  auto* loadBtn = new QPushButton(QStringLiteral("Open .misuc…"), this);
  status_ = new QLabel(this);
  status_->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextFaint.name()));
  actions->addWidget(saveBtn);
  actions->addWidget(loadBtn);
  actions->addWidget(status_);
  actions->addStretch();
  root->addLayout(actions);

  connect(saveBtn, &QPushButton::clicked, this, &ProjectPage::save);
  connect(loadBtn, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open Misuc project"), QString(),
        QStringLiteral("Misuc project (*.misuc)"));
    if (!path.isEmpty())
      loadPicked(path);
  });

  root->addStretch();
}

void ProjectPage::save() {
  session_.setName(nameEdit_->text().toStdString());
  const double tempo = tempoEdit_->text().toDouble();
  if (tempo >= 20.0 && tempo <= 400.0) {
    session_.setTempo(tempo);
    engine_.setTempo(tempo);
  }
  const QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Save Misuc project"), QString(),
      QStringLiteral("Misuc project (*.misuc)"));
  if (path.isEmpty())
    return;
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) {
    QMessageBox::warning(this, QStringLiteral("Save failed"), f.errorString());
    return;
  }
  f.write(QByteArray::fromStdString(session_.toJson()));
  status_->setText(QStringLiteral("Saved %1").arg(path));
}

void ProjectPage::loadPicked(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(this, QStringLiteral("Open failed"), f.errorString());
    return;
  }
  try {
    session_ =
        misuc::project::Session::fromJson(f.readAll().toStdString());
  } catch (const std::exception& e) {
    QMessageBox::warning(this, QStringLiteral("Invalid project"),
                         QString::fromLatin1(e.what()));
    return;
  }
  nameEdit_->setText(QString::fromStdString(session_.name()));
  tempoEdit_->setText(QString::number(session_.tempo()));
  engine_.setTempo(session_.tempo());
  status_->setText(QStringLiteral("Opened %1").arg(path));
}

} // namespace misuc::ui