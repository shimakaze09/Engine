// Declares command history types and APIs for the Engine editor tool.

#pragma once
#include <cstddef>
namespace engine::editor {

// Abstract editor command.
struct EditorCommand {
  /// Handles editor command.
  virtual ~EditorCommand() = default;  // OK to have vtable here (editor only)
  /// Handles execute.
  virtual void execute() noexcept = 0;
  /// Handles undo.
  virtual void undo() noexcept = 0;
  /// Handles redo.
  virtual void redo() noexcept { execute(); }
};

// Stack-based undo/redo history. Max kMaxHistory commands.
class CommandHistory final {
public:
  static constexpr std::size_t kMaxHistory = 64U;

  // Execute a command and push it on the undo stack. Clears redo stack.
  // Takes ownership of the command pointer (must be allocated with new(nothrow)).
  void execute(EditorCommand *cmd) noexcept;
  /// Handles undo.
  void undo() noexcept;
  /// Handles redo.
  void redo() noexcept;
  /// Returns whether can undo.
  bool can_undo() const noexcept;
  /// Returns whether can redo.
  bool can_redo() const noexcept;
  /// Handles clear.
  void clear() noexcept;

private:
  EditorCommand *m_history[kMaxHistory] = {};
  int m_top = -1;       // index of last executed command
  int m_count = 0;      // total valid entries in history
};

} // namespace engine::editor
