// Declares command history types and APIs for the Engine editor tool.

#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
namespace engine::editor {

// Abstract editor command. Each operation reports whether it fully
// applied; a false return promises the world was left unchanged (commands
// roll back their own partial work), so the history cursor only moves on
// complete transitions.
struct EditorCommand {
  virtual ~EditorCommand() = default;  // OK to have vtable here (editor only)
  /// Applies the edit; false when it could not (fully) apply.
  virtual bool execute() noexcept = 0;
  /// Reverts the edit; false when it could not (fully) revert.
  virtual bool undo() noexcept = 0;
  /// Re-applies after an undo (defaults to execute()).
  virtual bool redo() noexcept { return execute(); }
  /// Bytes this command holds, itself and what it owns, charged against
  /// the history's byte budget.
  virtual std::size_t memory_bytes() const noexcept = 0;
};

// Undo/redo history bounded by bytes, not by a count, as Unreal's undo
// buffer is: any number of edits stays undoable until the commands it
// holds pass the budget. Only then is the oldest dropped, and the first
// such drop since the history was last cleared logs a Warning, so an
// author learns history ran out rather than finding Ctrl+Z silently
// stopping short. The newest command is always kept, whatever its size.
class CommandHistory final {
public:
  /// The default budget: about 45,000 Inspector edits.
  static constexpr std::size_t kDefaultBudgetBytes = 256U * 1024U * 1024U;

  CommandHistory() noexcept = default;
  ~CommandHistory() noexcept = default;

  CommandHistory(const CommandHistory &) = delete;
  CommandHistory &operator=(const CommandHistory &) = delete;
  CommandHistory(CommandHistory &&) = delete;
  CommandHistory &operator=(CommandHistory &&) = delete;

  // Execute a command; on success push it on the undo stack and clear the
  // redo stack. A failed execute frees the command and leaves the history
  // (including the redo stack) untouched. Takes ownership of the command
  // pointer (must be allocated with new(nothrow)). The history's storage
  // grows here, at the end of a user gesture; when it cannot grow, the
  // oldest command is dropped as if the budget were reached.
  bool execute(EditorCommand *cmd) noexcept;
  /// Undoes the most recent command; the cursor moves only when the undo
  /// fully applied. False when empty or the undo failed.
  bool undo() noexcept;
  /// Re-executes the most recently undone command; the cursor moves only
  /// when the redo fully applied. False when none or the redo failed.
  bool redo() noexcept;
  /// Returns whether can undo.
  bool can_undo() const noexcept;
  /// Returns whether can redo.
  bool can_redo() const noexcept;
  /// Drops all undo/redo history.
  void clear() noexcept;

  /// Sets the byte budget (at least one command is always kept) and drops
  /// the oldest commands now if the history holds more.
  void set_budget_bytes(std::size_t bytes) noexcept;
  /// The byte budget in force.
  std::size_t budget_bytes() const noexcept { return m_budgetBytes; }
  /// Bytes the commands in the history hold now.
  std::size_t used_bytes() const noexcept { return m_usedBytes; }
  /// Commands in the history, undoable and redoable.
  std::size_t command_count() const noexcept { return m_count; }
  /// Commands dropped for the budget since the history was last cleared.
  std::size_t evicted_count() const noexcept { return m_evictedCount; }

  /// Opaque id of the command instance now at the undo cursor; with
  /// nothing left to undo it is 0 for a history that never dropped a
  /// command, and the dropped command's id once one was dropped, since
  /// that edit stays applied. Tokens are assigned once per execute() call
  /// and never reused, so scene-document dirty tracking can compare
  /// against a saved token and never falsely read "clean" for a state
  /// the history can no longer reach.
  std::uint64_t current_token() const noexcept;

private:
  /// One history slot: the command, its token and the bytes it was charged.
  struct Entry final {
    std::unique_ptr<EditorCommand> command{};
    std::uint64_t token = 0U;
    std::size_t bytes = 0U;
  };

  Entry &at(std::size_t index) noexcept;
  const Entry &at(std::size_t index) const noexcept;
  bool grow() noexcept;
  void evict_oldest() noexcept;

  std::unique_ptr<Entry[]> m_entries{};
  std::size_t m_capacity = 0U;
  std::size_t m_head = 0U;     // slot of the oldest command
  std::size_t m_count = 0U;    // commands held, undoable and redoable
  std::size_t m_undoable = 0U; // commands below the cursor
  std::size_t m_usedBytes = 0U;
  std::size_t m_budgetBytes = kDefaultBudgetBytes;
  std::size_t m_evictedCount = 0U;
  std::uint64_t m_floorToken = 0U; // token with nothing left to undo
  std::uint64_t m_nextToken = 1U;  // 0 is reserved for "empty history"
};

} // namespace engine::editor
