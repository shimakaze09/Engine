// Declares Lua scene management bindings for the Engine scripting system.

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Capacity of the deferred scene path buffer, shared by the binding state
/// and by any checkpoint of it so the two cannot drift.
inline constexpr std::size_t kPendingScenePathCapacity = 512U;

/// Identifies the deferred scene operation requested from Lua.
enum class SceneOp : std::uint8_t { None, Load, New };

/// A checkpoint of the deferred scene request, taken so an operation whose
/// failure must leave no trace can put the request back exactly as it was.
/// It is a value copy, not a handle: restoring it overwrites whatever the
/// failed operation queued, including a request it made from nothing.
struct PendingSceneOpCheckpoint {
  SceneOp op = SceneOp::None;
  char path[kPendingScenePathCapacity] = {};
};

/// Registers scene save/load/new functions on the engine Lua table.
void register_scene_bindings(lua_State *state) noexcept;

/// Clears any pending scene operation requested from Lua.
void reset_scene_bindings() noexcept;

/// Copies the deferred scene request out for later restoration.
PendingSceneOpCheckpoint capture_pending_scene_op() noexcept;

/// Puts a captured deferred scene request back, discarding whatever is
/// queued now. The captured value was validated when it was first queued,
/// so restoration re-validates nothing.
void restore_pending_scene_op(
    const PendingSceneOpCheckpoint &checkpoint) noexcept;

/// Marks that a scene-transition on_end_play dispatch (#198) is running so
/// a handler's own load_scene/new_scene request is rejected with a logged
/// warning instead of silently corrupting the transition already
/// committing. Called only from
/// dispatch_entity_scripts_end_for_transition().
void begin_scene_teardown_dispatch() noexcept;
/// Ends the reentrancy window opened by begin_scene_teardown_dispatch.
void end_scene_teardown_dispatch() noexcept;

} // namespace engine::scripting
