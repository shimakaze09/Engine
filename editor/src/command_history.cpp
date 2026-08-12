// Implements command history behavior for the Engine editor tool.

#include "engine/editor/command_history.h"

#include <cstddef>
#include <memory>
#include <utility>

namespace engine::editor {

bool CommandHistory::execute(EditorCommand *cmd) noexcept {
  if (cmd == nullptr) {
    return false;
  }

  // A failed execute must leave history untouched — dropping the redo
  // stack for an edit that never happened would corrupt the cursor.
  std::unique_ptr<EditorCommand> ownedCommand(cmd);
  if (!ownedCommand->execute()) {
    return false;
  }

  for (int i = m_top + 1; i < m_count; ++i) {
    m_history[static_cast<std::size_t>(i)].reset();
  }
  m_count = m_top + 1;

  if (m_count < static_cast<int>(kMaxHistory)) {
    m_history[static_cast<std::size_t>(m_count)] = std::move(ownedCommand);
    ++m_count;
    m_top = m_count - 1;
  } else {
      for (std::size_t i = 0U; i < kMaxHistory - 1U; ++i) {
      m_history[i] = std::move(m_history[i + 1U]);
    }
    m_history[kMaxHistory - 1U] = std::move(ownedCommand);
    m_top = static_cast<int>(kMaxHistory) - 1;
  }
  return true;
}

bool CommandHistory::undo() noexcept {
  if (m_top < 0) {
    return false;
  }
  EditorCommand *const cmd = m_history[static_cast<std::size_t>(m_top)].get();
  if ((cmd == nullptr) || !cmd->undo()) {
    return false;
  }
  --m_top;
  return true;
}

bool CommandHistory::redo() noexcept {
  if (m_top + 1 >= m_count) {
    return false;
  }
  EditorCommand *const cmd =
      m_history[static_cast<std::size_t>(m_top + 1)].get();
  if ((cmd == nullptr) || !cmd->redo()) {
    return false;
  }
  ++m_top;
  return true;
}

bool CommandHistory::can_undo() const noexcept {
  return m_top >= 0;
}

bool CommandHistory::can_redo() const noexcept {
  return m_top + 1 < m_count;
}

void CommandHistory::clear() noexcept {
  for (int i = 0; i < m_count; ++i) {
    m_history[static_cast<std::size_t>(i)].reset();
  }
  m_top = -1;
  m_count = 0;
}

} // namespace engine::editor
