#include "MisucCommandRegistry.h"

namespace misuc::ui::core {

MisucCommandRegistry& MisucCommandRegistry::instance() {
  static MisucCommandRegistry reg;
  return reg;
}

void MisucCommandRegistry::registerCommand(MisucCommand cmd) {
  commands_[cmd.id] = std::move(cmd);
}

const MisucCommand* MisucCommandRegistry::command(const QString& id) const {
  const auto it = commands_.find(id);
  return it == commands_.end() ? nullptr : &it->second;
}

MisucCommand* MisucCommandRegistry::command(const QString& id) {
  const auto it = commands_.find(id);
  return it == commands_.end() ? nullptr : &it->second;
}

std::vector<QString> MisucCommandRegistry::idsInCategory(const QString& category) const {
  std::vector<QString> out;
  for (const auto& [id, cmd] : commands_)
    if (cmd.category == category)
      out.push_back(id);
  return out;
}

std::vector<QString> MisucCommandRegistry::allIds() const {
  std::vector<QString> out;
  out.reserve(commands_.size());
  for (const auto& [id, cmd] : commands_)
    out.push_back(id);
  return out;
}

bool MisucCommandRegistry::execute(const QString& id) {
  MisucCommand* cmd = command(id);
  if (cmd == nullptr || !cmd->trigger)
    return false;
  if (cmd->enabled && !cmd->enabled())
    return false;
  cmd->trigger();
  return true;
}

bool MisucCommandRegistry::isCheckable(const QString& id) const {
  const MisucCommand* cmd = command(id);
  return cmd != nullptr && cmd->checked != nullptr;
}

bool MisucCommandRegistry::isChecked(const QString& id) const {
  const MisucCommand* cmd = command(id);
  return cmd != nullptr && cmd->checked && cmd->checked();
}

bool MisucCommandRegistry::isEnabled(const QString& id) const {
  const MisucCommand* cmd = command(id);
  if (cmd == nullptr)
    return false;
  return !cmd->enabled || cmd->enabled();
}

QString MisucCommandRegistry::displayName(const QString& id) const {
  const MisucCommand* cmd = command(id);
  return cmd ? cmd->name : id;
}

QString MisucCommandRegistry::shortcutText(const QString& id) const {
  const MisucCommand* cmd = command(id);
  if (cmd == nullptr || cmd->shortcut.isEmpty())
    return QString();
  return cmd->shortcut.toString(QKeySequence::NativeText);
}

} // namespace misuc::ui::core