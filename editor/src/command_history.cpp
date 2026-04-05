#include "engine/editor/command_history.h"

#include <cstddef>
#include <cstring>

namespace engine::editor {

void CommandHistory::execute(EditorCommand *cmd) noexcept {
  if (cmd == nullptr) {
    return;
  }

  cmd->execute();

  // Delete redo entries (entries beyond m_top).
  for (int i = m_top + 1; i < m_count; ++i) {
    delete m_history[i];
    m_history[i] = nullptr;
  }
  m_count = m_top + 1;

  if (m_count < static_cast<int>(kMaxHistory)) {
    m_history[m_count] = cmd;
    ++m_count;
    m_top = m_count - 1;
  } else {
    // At capacity: discard oldest entry, shift, insert at top.
    delete m_history[0];
    for (std::size_t i = 0U; i < kMaxHistory - 1U; ++i) {
      m_history[i] = m_history[i + 1U];
    }
    m_history[kMaxHistory - 1U] = cmd;
    m_top = static_cast<int>(kMaxHistory) - 1;
    // m_count stays kMaxHistory
  }
}

void CommandHistory::undo() noexcept {
  if (m_top < 0) {
    return;
  }
  m_history[m_top]->undo();
  --m_top;
}

void CommandHistory::redo() noexcept {
  if (m_top + 1 >= m_count) {
    return;
  }
  ++m_top;
  m_history[m_top]->redo();
}

bool CommandHistory::can_undo() const noexcept {
  return m_top >= 0;
}

bool CommandHistory::can_redo() const noexcept {
  return m_top + 1 < m_count;
}

void CommandHistory::clear() noexcept {
  for (int i = 0; i < m_count; ++i) {
    delete m_history[i];
    m_history[i] = nullptr;
  }
  m_top = -1;
  m_count = 0;
}

} // namespace engine::editor
