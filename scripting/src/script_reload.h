// Declares the one transaction every Lua hot reload runs its chunk under:
// the main script's reload and an entity module's alike. The chunk's
// top-level bindings are snapshotted, its other externally visible
// effects are staged in the reload scope (reload_transaction.h), and all
// of it commits only once the chunk has run without error, within its
// instruction budget, and returned what its caller requires; any failure
// leaves the state as it was before the reload began.

#pragma once

#include <cstdint>

struct lua_State;

namespace engine::scripting {

/// Outcome of run_chunk_as_reload.
enum class ReloadOutcome : std::uint8_t {
  /// Every effect of the chunk applied exactly once.
  Committed,
  /// The chunk failed, overran its budget or returned what the check
  /// refused, and nothing of it remains.
  RolledBack,
  /// The chunk's bindings are in place, but at least one staged effect
  /// could not apply at commit (logged with the count).
  CommitFailed,
  /// Another reload's scope is open, so this one did not start; nothing
  /// ran, and the caller may try again later.
  Busy,
};

/// Inspects the chunk's results, which are the top `results` values on
/// the stack, before the reload commits; false rolls it back. Must leave
/// the stack as it found it.
using ReloadResultCheck = bool (*)(lua_State *state, void *userData) noexcept;

/// Runs the loaded chunk on top of the stack as one reload. The chunk
/// commits only when it returns without error, within its instruction
/// budget, and `check` (when given) accepts its results; until then its
/// top-level bindings are snapshotted and its other effects staged, and a
/// failure discards them. On Committed or CommitFailed the chunk's
/// `results` values are left on the stack; otherwise the chunk is popped
/// and nothing is left.
ReloadOutcome run_chunk_as_reload(lua_State *state, const char *label,
                                  int results, ReloadResultCheck check,
                                  void *userData) noexcept;

} // namespace engine::scripting
