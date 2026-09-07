#pragma once

#include <QIcon>
#include <QLabel>
#include <QObject>
#include <QString>
#include <functional>
#include <unordered_map>

class QWidget;

namespace misuc::ui {

// A single contextual hint entry shown in the global HintBar. `value` may come
// from a live provider so meters/positions update on the UI poll tick. When
// `commandId` is set the record is resolved from the MisucCommandRegistry on
// every show/refresh, so hint text, shortcut, icon and live state are always
// the single source of truth (spec §78).
struct Hint {
  QString name;        // e.g. "Transport: Play"
  QString description; // e.g. "Start playback from the playhead"
  QString shortcut;    // e.g. "Space"
  QIcon icon;          // optional lead glyph
  QString commandId;   // registry-backed widget (optional)
  std::function<QString()> value; // live value provider, may be null
};

// Builds a Hint for the given registry command id (empty on unknown ids).
Hint hintFromCommand(const QString& commandId);

// Renders the hint line: [icon] [name — description]   [value]   [shortcut].
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
// value providers and registry state (called from the UI poll timer).
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
  // Registers a widget whose hint always comes from the command registry.
  void registerCommand(QWidget* widget, const QString& commandId);

  void setDefaultHint(Hint hint);
  void refresh();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  Hint hintFor(const QWidget* widget) const;

  HintBar* bar_;
  Hint defaultHint_;
  std::unordered_map<QWidget*, Hint> hints_;
  std::unordered_map<QWidget*, QString> commandIds_;
  QWidget* hovered_ = nullptr;
};

} // namespace misuc::ui