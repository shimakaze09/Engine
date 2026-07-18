// Declares command history types and APIs for the Engine editor tool.

#pragma once
#include <array>
#include <cstddef>
#include <memory>
namespace engine::editor {

// Abstract editor command.
struct EditorCommand {
  virtual ~EditorCommand() = default;  // OK to have vtable here (editor only)
  /// Applies the edit.
  virtual void execute() noexcept = 0;
  /// Reverts the edit.
  virtual void undo() noexcept = 0;
  /// Re-applies after an undo (defaults to execute()).
  virtual void redo() noexcept { execute(); }
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

  // Execute a command and push it on the undo stack. Clears redo stack.
  // Takes ownership of the command pointer (must be allocated with new(nothrow)).
  void execute(EditorCommand *cmd) noexcept;
  /// Undoes the most recent command; no-op when empty.
  void undo() noexcept;
  /// Re-executes the most recently undone command; no-op when none.
  void redo() noexcept;
  /// Returns whether can undo.
  bool can_undo() const noexcept;
  /// Returns whether can redo.
  bool can_redo() const noexcept;
  /// Drops all undo/redo history.
  void clear() noexcept;

private:
  std::array<std::unique_ptr<EditorCommand>, kMaxHistory> m_history{};
  int m_top = -1;       // index of last executed command
  int m_count = 0;      // total valid entries in history
};

} // namespace engine::editor
