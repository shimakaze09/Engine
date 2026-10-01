// Verifies CommandHistory::current_token() as the dirty-tracking primitive
// (issue #158): a saved-state marker compared against this token must
// clear dirty exactly when undo/redo returns to the saved position, and
// must never falsely read clean once the history drops commands past its
// byte budget: with nothing left to undo, the token is the newest dropped
// command's, whose edit stays applied.

#include "engine/editor/command_history.h"

#include <cstdio>
#include <new>

namespace {

/// Minimal always-succeeding command; only token bookkeeping is under
/// test here, not command payloads.
struct NoopCommand final : engine::editor::EditorCommand {
  bool execute() noexcept override { return true; }
  bool undo() noexcept override { return true; }
  std::size_t memory_bytes() const noexcept override { return 64U; }
};

engine::editor::EditorCommand *make_command() noexcept {
  return new (std::nothrow) NoopCommand();
}

/// EXPECTATION: a fresh history reports token 0; execute() advances to a
/// non-zero, unique token per command.
int check_fresh_and_advancing_tokens() {
  engine::editor::CommandHistory history;
  if (history.current_token() != 0U) {
    return 1;
  }

  if (!history.execute(make_command())) {
    return 2;
  }
  const std::uint64_t firstToken = history.current_token();
  if (firstToken == 0U) {
    return 3;
  }

  if (!history.execute(make_command())) {
    return 4;
  }
  const std::uint64_t secondToken = history.current_token();
  if ((secondToken == 0U) || (secondToken == firstToken)) {
    return 5;
  }

  return 0;
}

/// EXPECTATION: undoing back to a previously saved position reproduces
/// the exact same token (the "clean again" case), and redoing forward
/// reproduces the token that was current right before the undo.
int check_undo_redo_reproduces_saved_token() {
  engine::editor::CommandHistory history;

  if (!history.execute(make_command())) {
    return 10;
  }
  const std::uint64_t savedToken = history.current_token();

  if (!history.execute(make_command())) {
    return 11;
  }
  const std::uint64_t dirtyToken = history.current_token();
  if (dirtyToken == savedToken) {
    return 12;
  }

  if (!history.undo()) {
    return 13;
  }
  if (history.current_token() != savedToken) {
    return 14;
  }

  if (!history.redo()) {
    return 15;
  }
  if (history.current_token() != dirtyToken) {
    return 16;
  }

  return 0;
}

/// EXPECTATION: undoing every command returns to the token-0 empty-history
/// state, matching a document saved before any command was recorded.
int check_full_undo_returns_to_empty_token() {
  engine::editor::CommandHistory history;

  if (!history.execute(make_command()) || !history.execute(make_command())) {
    return 20;
  }
  if (!history.undo() || !history.undo()) {
    return 21;
  }
  if (history.current_token() != 0U) {
    return 22;
  }
  if (history.can_undo()) {
    return 23;
  }

  return 0;
}

/// EXPECTATION: once the history drops commands past its budget, undoing
/// everything left lands on the newest dropped command's token, never on
/// the empty-history 0: a document saved before any edit stays dirty
/// (the dropped edits remain applied), while one saved exactly at the
/// dropped position correctly reads clean there, since that is the state
/// the world is back in.
int check_evicted_token_never_reproduced() {
  engine::editor::CommandHistory history;
  history.set_budget_bytes(64U * 10U);
  const std::uint64_t savedFresh = history.current_token();

  std::uint64_t tokens[12] = {};
  for (std::size_t i = 0U; i < 12U; ++i) {
    if (!history.execute(make_command())) {
      return 30;
    }
    tokens[i] = history.current_token();
  }
  if (history.command_count() != 10U) {
    return 31;
  }

  // The first two were dropped; the second is the floor.
  std::uint64_t token = history.current_token();
  while (history.can_undo()) {
    if (!history.undo()) {
      return 33;
    }
    token = history.current_token();
    if (token == tokens[0]) {
      return 34;
    }
  }
  if (token == savedFresh) {
    return 32;
  }
  if (token != tokens[1]) {
    return 35;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  struct NamedCheck {
    const char *name;
    int (*fn)();
  };
  const NamedCheck checks[] = {
      {"check_fresh_and_advancing_tokens", &check_fresh_and_advancing_tokens},
      {"check_undo_redo_reproduces_saved_token",
       &check_undo_redo_reproduces_saved_token},
      {"check_full_undo_returns_to_empty_token",
       &check_full_undo_returns_to_empty_token},
      {"check_evicted_token_never_reproduced",
       &check_evicted_token_never_reproduced},
  };

  for (const auto &check : checks) {
    const int result = check.fn();
    if (result != 0) {
      std::fprintf(stderr, "command_history_token_test: %s failed: %d\n",
                   check.name, result);
      return result;
    }
  }

  std::printf("command_history_token_test: all tests passed\n");
  return 0;
}
