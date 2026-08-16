// Verifies CommandHistory::current_token() as the dirty-tracking primitive
// (issue #158): a saved-state marker compared against this token must
// clear dirty exactly when undo/redo returns to the saved position, and
// must never falsely read clean once that position is evicted by history
// growth beyond capacity.

#include "engine/editor/command_history.h"

#include <cstdio>
#include <new>

namespace {

/// Minimal always-succeeding command; only token bookkeeping is under
/// test here, not command payloads.
struct NoopCommand final : engine::editor::EditorCommand {
  bool execute() noexcept override { return true; }
  bool undo() noexcept override { return true; }
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

/// EXPECTATION: once a saved position is pushed out of the fixed-size ring
/// by later commands (capacity eviction), no future undo/redo can ever
/// reproduce that token again — the document must stay permanently dirty
/// until the next explicit save, never falsely read clean.
int check_evicted_token_never_reproduced() {
  engine::editor::CommandHistory history;

  if (!history.execute(make_command())) {
    return 30;
  }
  const std::uint64_t evictedToken = history.current_token();

  // Fill well past kMaxHistory so the first command's slot is recycled.
  for (std::size_t i = 0U;
       i < (engine::editor::CommandHistory::kMaxHistory * 2U); ++i) {
    if (!history.execute(make_command())) {
      return 31;
    }
  }

  // Walk every reachable position via undo and confirm the evicted token
  // never reappears. Undoing all the way to the token-0 sentinel is
  // itself expected here (it means every ring-resident command was
  // undone) and correctly still reads dirty against evictedToken, since
  // the commands the ring silently dropped were never undone and remain
  // permanently applied underneath.
  std::uint64_t token = history.current_token();
  if (token == evictedToken) {
    return 32;
  }
  while (history.can_undo()) {
    if (!history.undo()) {
      return 33;
    }
    token = history.current_token();
    if (token == evictedToken) {
      return 34;
    }
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
