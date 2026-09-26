// Implements the Lua hot-reload transaction declared in script_reload.h:
// the snapshot and in-place restore of the top-level bindings a chunk can
// change, and the run that stages every other effect in the reload scope
// and commits or rolls back as one.

#include "script_reload.h"

#include "binding_util.h"
#include "debug_bindings.h"
#include "reload_transaction.h"
#include "scene_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
#include <cstdio>

#include "engine/core/logging.h"

namespace engine::scripting {

namespace {

/// Carries the globals snapshot registry reference across the protected
/// snapshot/restore trampolines.
struct GlobalsSnapshotArgs final {
  int ref = LUA_NOREF;
};

// Rollback covers tables reachable
// from _G through table fields down to this depth, plus the
// upvalue cells of every Lua closure met during that walk; deeper tables,
// C-closure upvalues, userdata, and registry-only state stay shared and
// are not rolled back. Upvalue restore is cell-identity based: Lua joins
// upvalues across closures created by the same enclosing function into
// one shared cell (lua_upvalueid names it), so each cell is snapshotted
// and restored exactly once through any one holder — lua_setupvalue
// writes through the shared cell, never rebinding it, which preserves the
// aliasing relationships the script depends on.
constexpr std::size_t kMaxReloadSnapshotDepth = 8U;

/// Replaces the table on top of the stack with its snapshot copy,
/// recording orig->copy in memo and copy->orig in rev; cycles reuse the
/// memoized copy, and depth/stack limits fall back to a shared reference.
/// Also records the table's metatable identity in metaIndex and
/// walks into it the same way, so a failed reload that swaps, clears, or
/// mutates a metatable (a common OOP class-table pattern) rolls back too.
/// Lua closures met along the walk are collected into closuresIndex
/// (fn -> true) for the upvalue snapshot pass.
void deep_snapshot_table(lua_State *state, int memoIndex, int revIndex,
                         int metaIndex, int closuresIndex,
                         std::size_t depth) noexcept {
  const int origIndex = lua_absindex(state, -1);
  lua_pushvalue(state, origIndex);
  lua_rawget(state, memoIndex);
  if (!lua_isnil(state, -1)) {
    lua_replace(state, origIndex);
    return;
  }
  lua_pop(state, 1);

  lua_newtable(state);
  const int copyIndex = lua_absindex(state, -1);
  lua_pushvalue(state, origIndex);
  lua_pushvalue(state, copyIndex);
  lua_rawset(state, memoIndex);
  lua_pushvalue(state, copyIndex);
  lua_pushvalue(state, origIndex);
  lua_rawset(state, revIndex);

  if (((depth + 1U) < kMaxReloadSnapshotDepth) &&
      (lua_checkstack(state, 8) != 0) &&
      (lua_getmetatable(state, origIndex) != 0)) {
    // Record the ORIGINAL metatable's identity before recursing — the
    // recursive call replaces this stack slot with the metatable's own
    // snapshot copy, which restore never needs directly (its fields
    // restore through its own memo entry; only the identity link back to
    // origIndex needs to survive here).
    lua_pushvalue(state, origIndex);
    lua_pushvalue(state, -2);
    lua_rawset(state, metaIndex);
    deep_snapshot_table(state, memoIndex, revIndex, metaIndex, closuresIndex,
                        depth + 1U);
    lua_pop(state, 1);
  }

  lua_pushnil(state);
  while (lua_next(state, origIndex) != 0) {
    if ((lua_istable(state, -1) != 0) &&
        ((depth + 1U) < kMaxReloadSnapshotDepth) &&
        (lua_checkstack(state, 8) != 0)) {
      deep_snapshot_table(state, memoIndex, revIndex, metaIndex, closuresIndex,
                          depth + 1U);
    } else if ((lua_isfunction(state, -1) != 0) &&
               (lua_iscfunction(state, -1) == 0)) {
      // Remember every reachable Lua closure (dedup in the hash
      // part, ordered in the array part — the upvalue pass appends while
      // iterating, so no lua_next runs over a growing table); C closures
      // stay owned by their bindings.
      lua_pushvalue(state, -1);
      lua_rawget(state, closuresIndex);
      const bool closureKnown = !lua_isnil(state, -1);
      lua_pop(state, 1);
      if (!closureKnown) {
        lua_pushvalue(state, -1);
        lua_pushboolean(state, 1);
        lua_rawset(state, closuresIndex);
        lua_pushvalue(state, -1);
        lua_rawseti(state, closuresIndex,
                    static_cast<lua_Integer>(lua_rawlen(state, closuresIndex)) +
                        1);
      }
    }
    lua_pushvalue(state, -2);
    lua_pushvalue(state, -2);
    lua_rawset(state, copyIndex);
    lua_pop(state, 1);
  }

  lua_replace(state, origIndex);
}

/// Protected trampoline: deep-copies the globals table (and every nested
/// table within the depth cap) into a memoized snapshot refed into the
/// registry, so allocation failure while snapshotting stays catchable.
int snapshot_globals_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<GlobalsSnapshotArgs *>(lua_touserdata(state, 1));
  lua_createtable(state, 5, 0);
  const int containerIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int memoIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int revIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int metaIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int closuresIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int cellsIndex = lua_absindex(state, -1);
  lua_newtable(state);
  const int holdersIndex = lua_absindex(state, -1);

  lua_pushglobaltable(state);
  deep_snapshot_table(state, memoIndex, revIndex, metaIndex, closuresIndex, 0U);
  lua_pop(state, 1);

  // Snapshot every collected closure's upvalue cells. Cells are
  // keyed by lua_upvalueid identity so a cell shared across closures is
  // recorded (and later restored) exactly once; a table-valued cell is
  // additionally deep-snapshotted so its contents roll back in place.
  lua_Integer holderCount = 0;
  for (lua_Integer closure = 1;
       closure <= static_cast<lua_Integer>(lua_rawlen(state, closuresIndex));
       ++closure) {
    lua_rawgeti(state, closuresIndex, closure);
    const int fnIndex = lua_absindex(state, -1);
    for (int upvalue = 1;; ++upvalue) {
      if (lua_checkstack(state, 10) == 0) {
        break;
      }
      const char *name = lua_getupvalue(state, fnIndex, upvalue);
      if (name == nullptr) {
        break;
      }
      void *cellId = lua_upvalueid(state, fnIndex, upvalue);

      lua_pushlightuserdata(state, cellId);
      lua_rawget(state, cellsIndex);
      const bool cellKnown = !lua_isnil(state, -1);
      lua_pop(state, 1);
      if (!cellKnown) {
        if (lua_istable(state, -1) != 0) {
          lua_pushvalue(state, -1);
          deep_snapshot_table(state, memoIndex, revIndex, metaIndex,
                              closuresIndex, 0U);
          lua_pop(state, 1);
        }
        lua_pushlightuserdata(state, cellId);
        lua_pushvalue(state, -2);
        lua_rawset(state, cellsIndex);
      }

      // holders[n] = {fn, upvalueIndex, cellId}: restore needs one live
      // closure per cell to write through.
      lua_createtable(state, 3, 0);
      lua_pushvalue(state, fnIndex);
      lua_rawseti(state, -2, 1);
      lua_pushinteger(state, upvalue);
      lua_rawseti(state, -2, 2);
      lua_pushlightuserdata(state, cellId);
      lua_rawseti(state, -2, 3);
      lua_rawseti(state, holdersIndex, ++holderCount);

      lua_pop(state, 1);
    }
    lua_pop(state, 1);
  }

  lua_pushvalue(state, memoIndex);
  lua_rawseti(state, containerIndex, 1);
  lua_pushvalue(state, revIndex);
  lua_rawseti(state, containerIndex, 2);
  lua_pushvalue(state, metaIndex);
  lua_rawseti(state, containerIndex, 3);
  lua_pushvalue(state, cellsIndex);
  lua_rawseti(state, containerIndex, 4);
  lua_pushvalue(state, holdersIndex);
  lua_rawseti(state, containerIndex, 5);
  lua_pop(state, 6);
  args->ref = luaL_ref(state, LUA_REGISTRYINDEX);
  return 0;
}

/// Captures globals (nested tables included, up to the depth cap) for
/// rollback after a failed reload; false (with the error logged) when
/// snapshotting itself fails.
bool snapshot_global_bindings(lua_State *state, int *outReference) noexcept {
  GlobalsSnapshotArgs args{};
  if (!protected_c_operation(state, &snapshot_globals_trampoline, &args, 0,
                             "hot_reload globals snapshot")) {
    return false;
  }
  *outReference = args.ref;
  return true;
}

/// Restores one snapshotted table in place from its copy: keys added
/// since the snapshot are removed, snapshot keys are reassigned, and
/// values that are copies of snapshotted tables map back (via rev) to the
/// original table object so shared references keep their identity.
void restore_table_in_place(lua_State *state, int revIndex, int origIndex,
                            int copyIndex) noexcept {
  lua_pushnil(state);
  while (lua_next(state, origIndex) != 0) {
    lua_pop(state, 1);
    lua_pushvalue(state, -1);
    lua_rawget(state, copyIndex);
    const bool wasPresent = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (!wasPresent) {
      lua_pushvalue(state, -1);
      lua_pushnil(state);
      lua_rawset(state, origIndex);
    }
  }

  lua_pushnil(state);
  while (lua_next(state, copyIndex) != 0) {
    lua_pushvalue(state, -2);
    if (lua_istable(state, -2) != 0) {
      lua_pushvalue(state, -2);
      lua_rawget(state, revIndex);
      if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushvalue(state, -2);
      }
    } else {
      lua_pushvalue(state, -2);
    }
    lua_rawset(state, origIndex);
    lua_pop(state, 1);
  }
}

/// Protected trampoline: restores every snapshotted table in place (the
/// globals table is one memo entry), so allocation failure while
/// rebuilding tables stays catchable.
int restore_globals_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<GlobalsSnapshotArgs *>(lua_touserdata(state, 1));
  const int originalTop = lua_gettop(state);
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->ref);
  if (!lua_istable(state, -1)) {
    lua_settop(state, originalTop);
    return 0;
  }
  const int containerIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 1);
  const int memoIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 2);
  const int revIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 3);
  const int metaIndex = lua_absindex(state, -1);
  if ((lua_istable(state, memoIndex) == 0) ||
      (lua_istable(state, revIndex) == 0) ||
      (lua_istable(state, metaIndex) == 0) ||
      (lua_checkstack(state, 16) == 0)) {
    lua_settop(state, originalTop);
    return 0;
  }

  lua_pushnil(state);
  while (lua_next(state, memoIndex) != 0) {
    const int origIndex = lua_absindex(state, -2);
    const int copyIndex = lua_absindex(state, -1);
    restore_table_in_place(state, revIndex, origIndex, copyIndex);
    // #115a: reattach the table's original metatable identity (nil clears
    // one the failed reload added); the metatable's own fields, if it is
    // itself a snapshotted table, were just restored by this same loop.
    lua_pushvalue(state, origIndex);
    lua_rawget(state, metaIndex);
    lua_setmetatable(state, origIndex);
    lua_pop(state, 1);
  }

  // Restore each snapshotted upvalue cell exactly once through any
  // one recorded holder — lua_setupvalue writes through the shared cell,
  // so every closure aliasing it sees the restored value and the sharing
  // relationship itself is untouched. Older snapshots carry no
  // holders/cells slots and skip this pass.
  lua_rawgeti(state, containerIndex, 4);
  const int cellsIndex = lua_absindex(state, -1);
  lua_rawgeti(state, containerIndex, 5);
  const int holdersIndex = lua_absindex(state, -1);
  if ((lua_istable(state, cellsIndex) != 0) &&
      (lua_istable(state, holdersIndex) != 0)) {
    lua_newtable(state);
    const int doneIndex = lua_absindex(state, -1);
    const auto holderCount =
        static_cast<lua_Integer>(lua_rawlen(state, holdersIndex));
    for (lua_Integer holder = 1; holder <= holderCount; ++holder) {
      lua_rawgeti(state, holdersIndex, holder);
      const int tripleIndex = lua_absindex(state, -1);
      lua_rawgeti(state, tripleIndex, 3);
      lua_pushvalue(state, -1);
      lua_rawget(state, doneIndex);
      const bool cellDone = !lua_isnil(state, -1);
      lua_pop(state, 1);
      if (cellDone) {
        lua_pop(state, 2);
        continue;
      }
      lua_pushvalue(state, -1);
      lua_pushboolean(state, 1);
      lua_rawset(state, doneIndex);

      lua_rawgeti(state, tripleIndex, 1);
      const int fnIndex = lua_absindex(state, -1);
      lua_rawgeti(state, tripleIndex, 2);
      const auto upvalue = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_pushvalue(state, -2);
      lua_rawget(state, cellsIndex);
      if (lua_setupvalue(state, fnIndex, upvalue) == nullptr) {
        lua_pop(state, 1); // unreachable index: setupvalue pops nothing
      }
      lua_pop(state, 3);
    }
    lua_pop(state, 1);
  }

  lua_settop(state, originalTop);
  return 0;
}

/// Restores snapshotted tables under protection; a restore that itself
/// hits allocation failure is logged (tables may then be partially
/// restored — unavoidable under OOM).
void restore_global_bindings(lua_State *state, int snapshotReference) noexcept {
  GlobalsSnapshotArgs args{};
  args.ref = snapshotReference;
  static_cast<void>(protected_c_operation(state, &restore_globals_trampoline,
                                          &args, 0,
                                          "hot_reload globals restore"));
}

} // namespace

ReloadOutcome run_chunk_as_reload(lua_State *state, const char *label,
                                  int results, ReloadResultCheck check,
                                  void *userData) noexcept {
  if (state == nullptr) {
    return ReloadOutcome::RolledBack;
  }
  const char *context = (label != nullptr) ? label : "hot_reload";

  int snapshotReference = LUA_NOREF;
  if (!snapshot_global_bindings(state, &snapshotReference)) {
    lua_pop(state, 1);
    return ReloadOutcome::RolledBack;
  }
  // Captured after the chunk is loaded and before it runs: loading executes
  // no chunk code, so this is the request as it stood before the reload.
  const PendingSceneOpCheckpoint sceneOpCheckpoint = capture_pending_scene_op();
  if (!begin_reload_transaction()) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "%.100s: another reload is in progress", context);
    core::log_message(core::LogLevel::Warning, "scripting", message);
    lua_pop(state, 1);
    luaL_unref(state, LUA_REGISTRYINDEX, snapshotReference);
    return ReloadOutcome::Busy;
  }
  arm_debug_lua_hook(state);
  bool failed = (lua_pcall(state, 0, results, 0) != LUA_OK);
  bool resultsOnStack = !failed;
  if (failed) {
    log_lua_error(state, context);
  } else if (debug_instruction_budget_exhausted()) {
    char message[160] = {};
    std::snprintf(message, sizeof(message),
                  "%.100s: CPU instruction budget exhausted", context);
    core::log_message(core::LogLevel::Error, "scripting", message);
    failed = true;
  } else if ((check != nullptr) && !check(state, userData)) {
    failed = true;
  }
  if (failed) {
    rollback_reload_transaction();
  }
  const ReloadCommit commit =
      failed ? ReloadCommit::Refused : commit_reload_transaction();
  if (commit == ReloadCommit::Refused) {
    if (resultsOnStack) {
      lua_pop(state, results);
    }
    restore_global_bindings(state, snapshotReference);
    restore_pending_scene_op(sceneOpCheckpoint);
    luaL_unref(state, LUA_REGISTRYINDEX, snapshotReference);
    return ReloadOutcome::RolledBack;
  }
  luaL_unref(state, LUA_REGISTRYINDEX, snapshotReference);
  return (commit == ReloadCommit::Applied) ? ReloadOutcome::Committed
                                           : ReloadOutcome::CommitFailed;
}

} // namespace engine::scripting
