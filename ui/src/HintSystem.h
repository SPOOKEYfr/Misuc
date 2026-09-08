#pragma once

#include <QIcon>
#include <QLabel>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <functional>
#include <optional>
#include <vector>

#include "branding/Icons.h"

class QWidget;

namespace misuc::ui {

// A single contextual hint entry shown in the global HintBar. `value` may come
// from a live provider so meters/positions update on the UI poll tick. When
// `commandId` is set the record is resolved from the MisucCommandRegistry on
// every show/refresh, so hint text, shortcut, icon and live state are always
// the single source of truth (spec §78).
struct Hint {
  QString name;        // short, unambiguous: "Play", "Record", "Channel Rack"
  QString description; // one-line purpose: "Start playback from the playhead"
  QString shortcut;    // e.g. "Space"
  QIcon icon;          // lead glyph, always present for chrome controls
  QString commandId;   // registry-backed widget (optional)
  std::function<QString()> value; // live value provider, may be null
};

// Builds a Hint for the given registry command id (empty on unknown ids).
Hint hintFromCommand(const QString& commandId);

// Compact contextual info panel: [icon] [name — one-line purpose]
//                                         [live value] [shortcut chip].
// A real layout of child labels (never hand-painted text) so nothing overlaps
// and the bar keeps readable at 100-125% scaling and small windows.
class HintBar : public QWidget {
  Q_OBJECT
public:
  explicit HintBar(QWidget* parent = nullptr);

  void showHint(const Hint& hint);
  void clearHint();

  // Read-only view of the entry currently painted (used by the UI auditors).
  QString currentName() const { return hint_.name; }
  bool hasHint() const { return !hint_.name.isEmpty(); }

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  void elideLabels();

  Hint hint_;
  QLabel* glyph_ = nullptr;
  QLabel* name_ = nullptr;
  QLabel* desc_ = nullptr;
  QLabel* value_ = nullptr;
  QLabel* chip_ = nullptr;
};

// Registry + event filter: hovering a registered widget surfaces its hint in
// the shared HintBar; leaving restores the default. refresh() re-evaluates all
// value providers and registry state (called from the UI poll timer).
//
// Lifecycle: registration is keyed by QPointer so a widget that is destroyed
// mid-session (theme re-applies, dock re-shuffle, teardown) is atomically
// dropped and can never have a resolver invoked on a dangling `this`.
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

  // Registers an icon-only tool: the same record as registerHint, but also
  // suppresses the native QToolTip so hints surface in the HintBar only.
  void registerTool(QWidget* widget, const QString& name,
                    const QString& description = QString(),
                    const QString& shortcut = QString(),
                    icons::IconId icon = icons::IconId::None,
                    std::function<QString()> value = nullptr);
  // Registers a custom-painted control (no child widgets): the resolver maps a
  // local hit position to the hint shown under that hotspot. The resolver is
  // only ever invoked while the widget is alive (QPointer-guarded) and must
  // treat its own state as possibly-stale.
  void registerCustom(QWidget* widget,
                      std::function<std::optional<Hint>(const QPoint&)> resolve);

  void setDefaultHint(Hint hint);
  void refresh();

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  struct Entry {
    QPointer<QWidget> widget;
    Hint hint;
    QString commandId;
    std::function<std::optional<Hint>(const QPoint&)> resolve;
  };
  int findEntry(const QWidget* widget) const;
  void purgeEntry(int index);
  Hint hintFor(const QWidget* widget, const QPoint& pos) const;
  void showFor(QWidget* widget);
  void settleHover();

  HintBar* bar_;
  Hint defaultHint_;
  std::vector<Entry> entries_;
  QPointer<QWidget> hovered_;
  QPointer<QWidget> focused_;
  QTimer* delay_ = nullptr;
  QPoint lastPos_;
};

} // namespace misuc::ui