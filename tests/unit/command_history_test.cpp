// Verifies editor command history ownership behavior.

#include "engine/editor/command_history.h"

#include <cstdio>
#include <new>
#include <type_traits>

namespace {

int g_liveCommands = 0;
int g_destroyedCommands = 0;
int g_executeCount = 0;
int g_undoCount = 0;
int g_redoCount = 0;

void reset_counts() noexcept {
  g_liveCommands = 0;
  g_destroyedCommands = 0;
  g_executeCount = 0;
  g_undoCount = 0;
  g_redoCount = 0;
}

struct CountingCommand final : engine::editor::EditorCommand {
  CountingCommand() noexcept { ++g_liveCommands; }

  ~CountingCommand() override {
    --g_liveCommands;
    ++g_destroyedCommands;
  }

  void execute() noexcept override { ++g_executeCount; }

  void undo() noexcept override { ++g_undoCount; }

  void redo() noexcept override { ++g_redoCount; }
};

engine::editor::EditorCommand *make_command() noexcept {
  return new (std::nothrow) CountingCommand();
}

int check_destructor_releases_commands() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    history.execute(make_command());
    history.execute(make_command());
    if (g_liveCommands != 2) {
      return 10;
    }
  }

  if ((g_liveCommands != 0) || (g_destroyedCommands != 2)) {
    return 11;
  }
  return 0;
}

int check_redo_entries_are_released() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    history.execute(make_command());
    history.execute(make_command());
    history.undo();
    if (!history.can_redo()) {
      return 20;
    }

    history.execute(make_command());
    if (history.can_redo()) {
      return 21;
    }
    if ((g_liveCommands != 2) || (g_destroyedCommands != 1)) {
      return 22;
    }
  }

  if ((g_liveCommands != 0) || (g_destroyedCommands != 3)) {
    return 23;
  }
  return 0;
}

int check_capacity_eviction_releases_oldest_command() noexcept {
  reset_counts();
  {
    engine::editor::CommandHistory history{};
    for (std::size_t i = 0U;
         i < engine::editor::CommandHistory::kMaxHistory + 1U; ++i) {
      history.execute(make_command());
    }

    if ((g_liveCommands !=
         static_cast<int>(engine::editor::CommandHistory::kMaxHistory)) ||
        (g_destroyedCommands != 1)) {
      return 30;
    }
  }

  if ((g_liveCommands != 0) ||
      (g_destroyedCommands !=
       static_cast<int>(engine::editor::CommandHistory::kMaxHistory + 1U))) {
    return 31;
  }
  return 0;
}

int check_undo_redo_dispatch() noexcept {
  reset_counts();
  engine::editor::CommandHistory history{};
  history.execute(make_command());

  if ((g_executeCount != 1) || !history.can_undo() || history.can_redo()) {
    return 40;
  }

  history.undo();
  if ((g_undoCount != 1) || history.can_undo() || !history.can_redo()) {
    return 41;
  }

  history.redo();
  if ((g_redoCount != 1) || !history.can_undo() || history.can_redo()) {
    return 42;
  }
  return 0;
}

} // namespace

static_assert(!std::is_copy_constructible_v<engine::editor::CommandHistory>);
static_assert(!std::is_copy_assignable_v<engine::editor::CommandHistory>);
static_assert(!std::is_move_constructible_v<engine::editor::CommandHistory>);
static_assert(!std::is_move_assignable_v<engine::editor::CommandHistory>);

int main() {
  int result = check_destructor_releases_commands();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_redo_entries_are_released();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_capacity_eviction_releases_oldest_command();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  result = check_undo_redo_dispatch();
  if (result != 0) {
    std::fprintf(stderr, "command_history_test failed: %d\n", result);
    return result;
  }

  std::printf("command_history_test: all tests passed\n");
  return 0;
}
