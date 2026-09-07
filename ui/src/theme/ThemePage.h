#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace misuc::ui {

// Appearance page: pick a preset, set a custom accent, toggle glass surfaces,
// and import/export .misuctheme files. All changes apply live via the theme
// engine so every page restyles immediately. Theme is global (not part of an
// undoable session edit).
class ThemePage : public QWidget {
  Q_OBJECT
public:
  explicit ThemePage(QWidget* parent = nullptr);

signals:
  void edited();

private:
  void refreshFromEngine();
  void applyTheme();
  void updateAccentSwatch();
  void refreshPreview();
  void pickAccent();
  void exportTheme();
  void importTheme();

  QComboBox* presetBox_ = nullptr;
  QPushButton* accentBtn_ = nullptr;
  QCheckBox* glassChk_ = nullptr;
  QLabel* preview_ = nullptr;
};

} // namespace misuc::ui