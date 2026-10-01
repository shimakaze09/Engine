// Implements command history behavior for the Engine editor tool.

#include "engine/editor/command_history.h"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <new>
#include <utility>

#include "engine/core/logging.h"

namespace engine::editor {

namespace {

/// Slots the history starts with; it doubles from here as it fills.
constexpr std::size_t kInitialCapacity = 64U;

} // namespace

CommandHistory::Entry &CommandHistory::at(std::size_t index) noexcept {
  return m_entries[(m_head + index) % m_capacity];
}

const CommandHistory::Entry &
CommandHistory::at(std::size_t index) const noexcept {
  return m_entries[(m_head + index) % m_capacity];
}

bool CommandHistory::grow() noexcept {
  const std::size_t capacity =
      (m_capacity == 0U) ? kInitialCapacity : (m_capacity * 2U);
  std::unique_ptr<Entry[]> entries(new (std::nothrow) Entry[capacity]);
  if (entries == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < m_count; ++i) {
    entries[i] = std::move(at(i));
  }
  m_entries = std::move(entries);
  m_capacity = capacity;
  m_head = 0U;
  return true;
}

void CommandHistory::evict_oldest() noexcept {
  Entry &oldest = at(0U);
  m_floorToken = oldest.token;
  m_usedBytes -= oldest.bytes;
  oldest = Entry{};
  m_head = (m_head + 1U) % m_capacity;
  --m_count;
  --m_undoable;
  ++m_evictedCount;
  if (m_evictedCount == 1U) {
    char message[256] = {};
    std::snprintf(message, sizeof(message),
                  "undo history is full (%zu MiB of edits): the oldest edits "
                  "can no longer be undone",
                  m_budgetBytes / (1024U * 1024U));
    core::log_message(core::LogLevel::Warning, "editor", message);
  }
}

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

  for (std::size_t i = m_undoable; i < m_count; ++i) {
    Entry &redoable = at(i);
    m_usedBytes -= redoable.bytes;
    redoable = Entry{};
  }
  m_count = m_undoable;

  if ((m_count == m_capacity) && !grow()) {
    // No room and no memory to grow: the oldest edit makes room, as the
    // budget would.
    if (m_count == 0U) {
      core::log_message(core::LogLevel::Error, "editor",
                        "undo history: out of memory; the edit was made "
                        "but cannot be undone");
      return true;
    }
    evict_oldest();
  }

  Entry &slot = at(m_count);
  slot.bytes = ownedCommand->memory_bytes();
  slot.token = m_nextToken++;
  slot.command = std::move(ownedCommand);
  m_usedBytes += slot.bytes;
  ++m_count;
  m_undoable = m_count;

  while ((m_usedBytes > m_budgetBytes) && (m_count > 1U)) {
    evict_oldest();
  }
  return true;
}

bool CommandHistory::undo() noexcept {
  if (m_undoable == 0U) {
    return false;
  }
  EditorCommand *const cmd = at(m_undoable - 1U).command.get();
  if ((cmd == nullptr) || !cmd->undo()) {
    return false;
  }
  --m_undoable;
  return true;
}

bool CommandHistory::redo() noexcept {
  if (m_undoable >= m_count) {
    return false;
  }
  EditorCommand *const cmd = at(m_undoable).command.get();
  if ((cmd == nullptr) || !cmd->redo()) {
    return false;
  }
  ++m_undoable;
  return true;
}

bool CommandHistory::can_undo() const noexcept { return m_undoable > 0U; }

bool CommandHistory::can_redo() const noexcept { return m_undoable < m_count; }

void CommandHistory::clear() noexcept {
  for (std::size_t i = 0U; i < m_count; ++i) {
    at(i) = Entry{};
  }
  m_head = 0U;
  m_count = 0U;
  m_undoable = 0U;
  m_usedBytes = 0U;
  m_evictedCount = 0U;
  m_floorToken = 0U;
  // m_nextToken is not reset: tokens must stay unique across the whole
  // session so a document saved before a clear() never reads as clean
  // against a numerically coincidental post-clear position.
}

void CommandHistory::set_budget_bytes(std::size_t bytes) noexcept {
  m_budgetBytes = bytes;
  // Only undoable commands are dropped from the front; a redoable tail is
  // newer than any of them and goes first only when nothing else is left.
  while ((m_usedBytes > m_budgetBytes) && (m_count > 1U) && (m_undoable > 0U)) {
    evict_oldest();
  }
}

std::uint64_t CommandHistory::current_token() const noexcept {
  if (m_undoable == 0U) {
    return m_floorToken;
  }
  return at(m_undoable - 1U).token;
}

} // namespace engine::editor
