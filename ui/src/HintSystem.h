#pragma once

#include <QLabel>
#include <QObject>
#include <QString>
#include <functional>
#include <unordered_map>

class QWidget;

namespace misuc::ui {

// A single contextual hint entry shown in the global HintBar. `value` may come
// from a live provider so meters/positions update on the UI poll tick.
struct Hint {
  QString name;        // e.g. "Transport: Play"
  QString description; // e.g. "Start playback from the playhead"
  QString shortcut;    // e.g. "Space"
  std::function<QString()> value; // live value provider, may be null
};

// Renders the hint line: [name — description]   value   [shortcut].
class HintBar : public QLabel {
  Q_OBJECT
public:
  explicit HintBar(QWidget* parent = nullptr);

  void showHint(const Hint& hint);
  void clearHint();

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  Hint hint_;
};

// Registry + event filter: hovering a registered widget surfaces its hint in
// the shared HintBar; leaving restores the default. refresh() re-evaluates all
// value providers (called from the UI poll timer).
class HintManager : public QObject {
  Q_OBJECT
public:
  explicit HintManager(HintBar* bar, QWidget* parent = nullptr);

  // Registers live hints for a widget (tooltip-like, replaces QToolTip).
  void registerHint(QWidget* widget, Hint hint);
  void registerHint(QWidget* widget, const QString& name,
                    const QString& description = QString(),
                    const QString& shortcut = QString(),
                    std::function<QString()> value = nullptr);

  void setDefaultHint(Hint hint);
  void refresh();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  HintBar* bar_;
  Hint defaultHint_;
  std::unordered_map<QWidget*, Hint> hints_;
  QWidget* hovered_ = nullptr;
};

} // namespace misuc::ui