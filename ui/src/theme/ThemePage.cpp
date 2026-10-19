#include "ThemePage.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "ThemeEngine.h"
#include "../Theme.h"
#include "HintSystem.h"

namespace misuc::ui {

namespace {

const char* kLayoutDescriptions[] = {
    "Studio home: browser left, inspector right, mixer below",
    "Arrangement: wide timeline, narrow browser, low mixer",
    "Mix-down: channel rack docked above the inspector on the right",
    "Note editing: piano roll central, rack + browser stacked left",
};

} // namespace

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

  auto* wlTitle = new QLabel(QStringLiteral("Workspace layout"), this);
  wlTitle->setFont(theme::uiFont(12, QFont::Bold));
  root->addWidget(wlTitle);
  auto* wlDesc = new QLabel(
      QStringLiteral("Real dock arrangements \u2014 panels stay movable, "
                     "resizable, floatable and closable in every preset. "
                     "Choosing a preset moves the panel dividers; dragging a "
                     "panel switches to a custom layout that Misuc remembers."),
      this);
  wlDesc->setStyleSheet(QStringLiteral("color: %1;").arg(theme::kTextDim.name()));
  wlDesc->setWordWrap(true);
  root->addWidget(wlDesc);

  layoutGrid_ = new QGridLayout();
  layoutGrid_->setSpacing(8);
  root->addLayout(layoutGrid_);

  auto* layoutRow = new QHBoxLayout();
  layoutRow->setSpacing(10);
  layoutStatus_ = new QLabel(this);
  layoutStatus_->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(theme::kTextDim.name()));
  layoutRow->addWidget(layoutStatus_, 1);
  resetWorkspaceBtn_ = new QPushButton(QStringLiteral("Reset Workspace"), this);
  resetWorkspaceBtn_->setCursor(Qt::PointingHandCursor);
  layoutRow->addWidget(resetWorkspaceBtn_);
  root->addLayout(layoutRow);

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

  // Refresh the UI when the theme engine changes the palette at runtime
  // (e.g. preset switch from another page, or accent color change).
  connect(&ThemeEngine::instance(), &ThemeEngine::themeChanged, this,
          [this] { refreshFromEngine(); });
  connect(saveBtn, &QPushButton::clicked, this, &ThemePage::exportTheme);
  connect(loadBtn, &QPushButton::clicked, this, &ThemePage::importTheme);
  connect(resetWorkspaceBtn_, &QPushButton::clicked, this,
          &ThemePage::workspaceResetRequested);

  refreshFromEngine();
}

void ThemePage::setupWorkspaceLayout(const QStringList& names) {
  while (QLayoutItem* item = layoutGrid_->takeAt(0))
    delete item->widget();
  layoutCards_.clear();

  for (int i = 0; i < names.size(); ++i) {
    auto* card = new QPushButton(names[i], this);
    card->setCheckable(true);
    card->setCursor(Qt::PointingHandCursor);
    card->setToolTip(i < 4 ? QString::fromUtf8(kLayoutDescriptions[i])
                           : QString());
    const int col = i % 2;
    const int row = i / 2;
    layoutGrid_->addWidget(card, row, col);
    layoutCards_.push_back(card);
    const QString name = names[i];
    connect(card, &QPushButton::clicked, this,
            [this, name] { onPresetCardClicked(name); });
    if (hints_)
      hints_->registerHint(card, QStringLiteral("Workspace Layout"),
          QStringLiteral("Switch to this panel arrangement"));
  }
  refreshWorkspacePreset(QStringLiteral("Classic"));
}

void ThemePage::refreshWorkspacePreset(const QString& current) {
  for (int i = 0; i < layoutCards_.size(); ++i) {
    const bool active = layoutCards_[i]->text() == current;
    layoutCards_[i]->setChecked(active);
  }
  if (!layoutStatus_)
    return;
  if (current == QStringLiteral("Custom")) {
    layoutStatus_->setText(
        QStringLiteral("Custom layout \u2014 you rearranged the panels. "
                       "Pick a preset above to reset the arrangement."));
    layoutStatus_->setStyleSheet(QStringLiteral("color: %1;")
                                     .arg(theme::kMeterHot.name()));
  } else {
    layoutStatus_->setText(
        QStringLiteral("%1 \u2014 panels are movable, resizable and "
                       "floatable in every preset.")
            .arg(current));
    layoutStatus_->setStyleSheet(QStringLiteral("color: %1;")
                                     .arg(theme::kTextDim.name()));
  }
}

void ThemePage::onPresetCardClicked(const QString& name) {
  emit workspacePresetSelected(name);
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

void ThemePage::attachHints(HintManager* hints) {
  hints_ = hints;
hints_->registerHint(presetBox_, QStringLiteral("Theme Preset"),
                     QStringLiteral("Pick a visual preset; changes apply live"));
hints_->registerHint(presetBox_, QStringLiteral("Theme Preset"),
                     QStringLiteral("Pick a visual preset; changes apply live"));
  hints_->registerHint(accentBtn_, QStringLiteral("Accent Color"),
                       QStringLiteral("Choose a custom accent color"));
  hints_->registerHint(glassChk_, QStringLiteral("Glass Surfaces"),
                       QStringLiteral("Toggle translucent window fills"));
  hints_->registerHint(preview_, QStringLiteral("Live Preview"),
                       QStringLiteral("Preview the active theme"));
  hints_->registerHint(resetWorkspaceBtn_, QStringLiteral("Reset Workspace"),
                       QStringLiteral("Restore the last layout preset"));
}

} // namespace misuc::ui