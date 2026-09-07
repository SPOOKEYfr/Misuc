#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QTimer;

namespace misuc::ui {

// Premium hover tooltip (spec §8): a floating smoked-glass panel that appears
// near the cursor after a short delay, follows the pointer, and never goes off
// screen. All text is sourced from the MisucCommandRegistry so the tooltip,
// HintBar, menus and shortcuts always agree. Instances can only be reached via
// MisucTooltip::attach; there is one global tool to avoid stacking artifacts.
class MisucTooltip final : public QWidget {
  Q_OBJECT
public:
  static MisucTooltip& instance();

  // Installs hover-tooltip behavior on `target` backed by the registry command
  // `commandId`. Safe to call multiple times; the previous command is replaced.
  static void attach(QWidget* target, const QString& commandId);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;
  void hideEvent(QHideEvent* event) override;

private:
  explicit MisucTooltip(QWidget* parent = nullptr);
  void scheduleShow();
  void showNow();
  void hideNow();
  void followCursor();
  void build(const QString& commandId);

  QLabel* title_ = nullptr;
  QLabel* body_ = nullptr;
  QLabel* meta_ = nullptr;
  QTimer* delay_ = nullptr;
  QTimer* mover_ = nullptr;
  QString commandId_;
  QPointer<QWidget> hovered_;
};

} // namespace misuc::ui