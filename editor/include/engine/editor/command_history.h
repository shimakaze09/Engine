#pragma once
#include <cstddef>
namespace engine::editor {

// Abstract editor command.
struct EditorCommand {
  virtual ~EditorCommand() = default;  // OK to have vtable here (editor only)
  virtual void execute() noexcept = 0;
  virtual void undo() noexcept = 0;
  virtual void redo() noexcept { execute(); }
};

// Stack-based undo/redo history. Max kMaxHistory commands.
class CommandHistory final {
public:
  static constexpr std::size_t kMaxHistory = 64U;

  // Execute a command and push it on the undo stack. Clears redo stack.
  // Takes ownership of the command pointer (must be allocated with new(nothrow)).
  void execute(EditorCommand *cmd) noexcept;
  void undo() noexcept;
  void redo() noexcept;
  bool can_undo() const noexcept;
  bool can_redo() const noexcept;
  void clear() noexcept;

private:
  EditorCommand *m_history[kMaxHistory] = {};
  int m_top = -1;       // index of last executed command
  int m_count = 0;      // total valid entries in history
};

} // namespace engine::editor
