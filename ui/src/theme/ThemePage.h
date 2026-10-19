#pragma once

#include <QWidget>
#include <QVector>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;

namespace misuc::ui {

class HintManager;

// Appearance page: pick a preset, set a custom accent, toggle glass surfaces,
// and import/export .misuctheme files. All changes apply live via the theme
// engine so every page restyles immediately. Theme is global (not part of an
// undoable session edit). Also hosts the WORKSPACE LAYOUT section: the four
// real layout presets (Classic / Arrangement / Production / Editing), a live
// Custom indicator, and Reset Workspace.
class ThemePage : public QWidget {
  Q_OBJECT
public:
  explicit ThemePage(QWidget* parent = nullptr);

  // Supplies the workspace layout preset names to render as selectable cards.
  void setupWorkspaceLayout(const QStringList& names);
  // Reflects the current layout state (a preset name, or "Custom").
  void refreshWorkspacePreset(const QString& current);

  // Registers hints for the preset, accent, glass, save/load and workspace
  // layout controls.
  void attachHints(HintManager* hints);

 signals:
  void edited();
  void workspacePresetSelected(const QString& name);
  void workspaceResetRequested();

private:
  void refreshFromEngine();
  void applyTheme();
  void updateAccentSwatch();
  void refreshPreview();
  void pickAccent();
  void exportTheme();
  void importTheme();

  void onPresetCardClicked(const QString& name);

  QComboBox* presetBox_ = nullptr;
  QPushButton* accentBtn_ = nullptr;
  QCheckBox* glassChk_ = nullptr;
  QLabel* preview_ = nullptr;

  // Hint registry; null until attachHints() is called from MainWindow::buildPages().
  HintManager* hints_ = nullptr;

  // Workspace layout section.
  QGridLayout* layoutGrid_ = nullptr;
  QVector<QPushButton*> layoutCards_;
  QLabel* layoutStatus_ = nullptr;
  QPushButton* resetWorkspaceBtn_ = nullptr;
};

} // namespace misuc::ui