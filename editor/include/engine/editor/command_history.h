// Declares command history types and APIs for the Engine editor tool.

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
namespace engine::editor {

// Abstract editor command. Each operation reports whether it fully
// applied; a false return promises the world was left unchanged (commands
// roll back their own partial work), so the history cursor only moves on
// complete transitions (issue #117).
struct EditorCommand {
  virtual ~EditorCommand() = default;  // OK to have vtable here (editor only)
  /// Applies the edit; false when it could not (fully) apply.
  virtual bool execute() noexcept = 0;
  /// Reverts the edit; false when it could not (fully) revert.
  virtual bool undo() noexcept = 0;
  /// Re-applies after an undo (defaults to execute()).
  virtual bool redo() noexcept { return execute(); }
};

// Stack-based undo/redo history. Max kMaxHistory commands.
class CommandHistory final {
public:
  static constexpr std::size_t kMaxHistory = 64U;

  CommandHistory() noexcept = default;
  ~CommandHistory() noexcept = default;

  CommandHistory(const CommandHistory &) = delete;
  CommandHistory &operator=(const CommandHistory &) = delete;
  CommandHistory(CommandHistory &&) = delete;
  CommandHistory &operator=(CommandHistory &&) = delete;

  // Execute a command; on success push it on the undo stack and clear the
  // redo stack. A failed execute frees the command and leaves the history
  // (including the redo stack) untouched. Takes ownership of the command
  // pointer (must be allocated with new(nothrow)).
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

  /// Opaque id of the command instance now at the undo cursor; 0 denotes
  /// the empty-history state (a fresh history, or one just cleared/undone
  /// all the way back). Tokens are assigned once per execute() call and
  /// never reused, including across the drop-oldest shift once history
  /// is full, so scene-document dirty tracking can compare against a
  /// saved token and never falsely read "clean" for a state execute()
  /// already discarded from the ring.
  std::uint64_t current_token() const noexcept;

private:
  std::array<std::unique_ptr<EditorCommand>, kMaxHistory> m_history{};
  std::array<std::uint64_t, kMaxHistory> m_tokens{};
  int m_top = -1;       // index of last executed command
  int m_count = 0;      // total valid entries in history
  std::uint64_t m_nextToken = 1U;  // 0 is reserved for "empty history"
};

} // namespace engine::editor
