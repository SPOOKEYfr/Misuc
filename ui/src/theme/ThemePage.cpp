#include "ThemePage.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "ThemeEngine.h"
#include "../Theme.h"

namespace misuc::ui {

ThemePage::ThemePage(QWidget* parent) : QWidget(parent) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(32, 24, 32, 24);
  root->setSpacing(16);

  auto* t = new QLabel(QStringLiteral("Appearance"), this);
  t->setFont(theme::uiFont(13, QFont::Bold));
  root->addWidget(t);
  auto* s = new QLabel(
      QStringLiteral("Pick a preset or set your own accent color. Glass "
                     "surfaces toggle the translucent fills. Themes can be "
                     "saved as .misuctheme and shared between machines."),
      this);
  s->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  s->setWordWrap(true);
  root->addWidget(s);

  auto* form = new QFormLayout();
  form->setSpacing(14);

  presetBox_ = new QComboBox(this);
  presetBox_->setObjectName(QStringLiteral("smallButton"));
  presetBox_->addItems(ThemeEngine::instance().presetNames());
  presetBox_->setMaximumWidth(280);
  form->addRow(QStringLiteral("Preset"), presetBox_);

  accentBtn_ = new QPushButton(this);
  accentBtn_->setMaximumWidth(280);
  accentBtn_->setCursor(Qt::PointingHandCursor);
  form->addRow(QStringLiteral("Accent"), accentBtn_);

  glassChk_ = new QCheckBox(QStringLiteral("Use glass surfaces"), this);
  glassChk_->setChecked(ThemeEngine::instance().glassEnabled());
  form->addRow(QString(), glassChk_);

  root->addLayout(form);

  auto* io = new QHBoxLayout();
  io->setSpacing(10);
  auto* saveBtn =
      new QPushButton(QStringLiteral("Save Theme\u2026"), this);
  auto* loadBtn =
      new QPushButton(QStringLiteral("Load Theme\u2026"), this);
  io->addWidget(saveBtn);
  io->addWidget(loadBtn);
  root->addLayout(io);

  auto* pcap = new QLabel(QStringLiteral("Live preview"), this);
  pcap->setStyleSheet(
      QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  root->addWidget(pcap);

  preview_ = new QLabel(this);
  preview_->setWordWrap(true);
  preview_->setMinimumHeight(120);
  root->addWidget(preview_);

  root->addStretch();

  connect(presetBox_, &QComboBox::currentTextChanged, this, [this](const QString& name) {
    if (!ThemeEngine::instance().setPreset(name))
      return;
    applyTheme();
  });
  connect(accentBtn_, &QPushButton::clicked, this, &ThemePage::pickAccent);
  connect(glassChk_, &QCheckBox::toggled, this, [this](bool on) {
    ThemeEngine::instance().setGlassEnabled(on);
    applyTheme();
  });
  connect(saveBtn, &QPushButton::clicked, this, &ThemePage::exportTheme);
  connect(loadBtn, &QPushButton::clicked, this, &ThemePage::importTheme);

  refreshFromEngine();
}

void ThemePage::refreshFromEngine() {
  auto& te = ThemeEngine::instance();
  const QSignalBlocker block(presetBox_);
  const int ix = presetBox_->findText(te.presetName());
  if (te.presetName() == QStringLiteral("Custom") && ix < 0) {
    presetBox_->addItem(QStringLiteral("Custom"));
    presetBox_->setCurrentIndex(presetBox_->count() - 1);
  } else if (ix >= 0) {
    presetBox_->setCurrentIndex(ix);
  }
  updateAccentSwatch();
  applyTheme();
}

void ThemePage::updateAccentSwatch() {
  accentBtn_->setText(QStringLiteral("  ") + ThemeEngine::instance().accent().name());
  accentBtn_->setStyleSheet(QStringLiteral(
      "QPushButton { background-color: %1; color: %2; border: 1px solid %3; "
      "border-radius: 6px; padding: 6px 10px; text-align: left center; }")
                                .arg(ThemeEngine::instance().accent().name(),
                                     theme::kText.name(),
                                     theme::kBorderBright.name()));
}

void ThemePage::applyTheme() {
  ThemeEngine::instance().apply(*qApp);
  updateAccentSwatch();
  refreshPreview();
  emit edited();
}

void ThemePage::refreshPreview() {
  const QColor bg = theme::kPanel;
  const QColor br = theme::kBorder;
  const QColor accent = theme::kAccent;
  preview_->setStyleSheet(QStringLiteral(
      "QLabel { background-color: %1; border: 1px solid %2; border-radius: "
      "10px; padding: 16px; } .preview-btn { background-color: %3; color: %4; "
      "border-radius: 6px; padding: 4px 10px; }")
                              .arg(bg.name(), br.name(), accent.name(),
                                   theme::kText.name()));
  preview_->setText(QStringLiteral(
      "<center style='font-size:12px;'>%1</center>"
      "<p style='color:%2;'>Panel sample with a %3 accent. Widgets, mixer "
      "strips, the piano roll and all pages restyle in one shot.</p>")
                        .arg(theme::kText.name())
                        .arg(theme::kTextDim.name())
                        .arg(accent.name()));
}

void ThemePage::pickAccent() {
  const QColor c = QColorDialog::getColor(ThemeEngine::instance().accent(),
                                          this, QStringLiteral("Accent color"));
  if (!c.isValid())
    return;
  ThemeEngine::instance().setAccent(c);
  applyTheme();
}

void ThemePage::exportTheme() {
  const QString path = QFileDialog::getSaveFileName(
      this, QStringLiteral("Save Misuc theme"), QString(),
      QStringLiteral("Misuc theme (*.misuctheme)"));
  if (path.isEmpty())
    return;
  QString err;
  if (!ThemeEngine::instance().saveToFile(path, err))
    QMessageBox::warning(this, QStringLiteral("Save failed"), err);
}

void ThemePage::importTheme() {
  const QString path = QFileDialog::getOpenFileName(
      this, QStringLiteral("Load Misuc theme"), QString(),
      QStringLiteral("Misuc theme (*.misuctheme)"));
  if (path.isEmpty())
    return;
  QString err;
  if (!ThemeEngine::instance().loadFromFile(path, err)) {
    QMessageBox::warning(this, QStringLiteral("Load failed"), err);
    return;
  }
  refreshFromEngine();
}

} // namespace misuc::ui