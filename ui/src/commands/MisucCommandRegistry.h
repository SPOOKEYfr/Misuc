#pragma once

#include <QKeySequence>
#include <QObject>
#include <QString>
#include <functional>
#include <unordered_map>
#include <vector>

#include "branding/Icons.h"

namespace misuc::ui::core {

// Central command table (spec §7, §51, §78): the MisucCommandRegistry is the
// single source of truth for every user command — its id, display name,
// description, keyboard shortcut, icon, category, dynamic state (checked /
// enabled), live value text and trigger. Menus, toolbars, the global Hint Bar,
// the hover tooltip and all shortcut handling are generated from these records,
// so a description or shortcut is never duplicated by hand in a widget.
struct MisucCommand {
  QString id;
  QString name;
  QString description;
  QString category;
  icons::IconId icon = icons::IconId::None;
  QKeySequence shortcut;
  std::function<bool()> checked = nullptr;  // optional checkable-state provider
  std::function<bool()> enabled = nullptr;  // optional availability provider
  std::function<QString()> value = nullptr; // optional live value for the HintBar
  std::function<void()> trigger = nullptr;
};

class MisucCommandRegistry final : public QObject {
  Q_OBJECT
public:
  static MisucCommandRegistry& instance();

  void registerCommand(MisucCommand cmd);
  const MisucCommand* command(const QString& id) const;
  MisucCommand* command(const QString& id);

  std::vector<QString> idsInCategory(const QString& category) const;
  std::vector<QString> allIds() const;

  // Runs the trigger when present; returns false if the command is unknown.
  bool execute(const QString& id);
  bool isCheckable(const QString& id) const;
  bool isChecked(const QString& id) const;
  bool isEnabled(const QString& id) const;
  QString displayName(const QString& id) const;
  QString shortcutText(const QString& id) const;

private:
  MisucCommandRegistry() = default;
  std::unordered_map<QString, MisucCommand> commands_;
};

} // namespace misuc::ui::core