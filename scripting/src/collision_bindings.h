// Declares private Lua collision and trigger callback bindings for the
// scripting module.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/entity.h"

struct lua_State;

namespace engine::scripting {

using PushEntityHandleFn = void (*)(lua_State *state,
                                    core::Entity entity) noexcept;

/// Lua binding: engine.on_collision_handler(callback). Returns the
/// handler's id, or nil and a reason: a non-function argument, or a full
/// table, whose first refusal logs a Warning.
int lua_engine_on_collision_register(lua_State *state) noexcept;
/// Lua binding: Lua engine.remove_collision_handler(handler_id).
int lua_engine_remove_collision_handler(lua_State *state) noexcept;

/// Releases all registered Lua collision callback refs.
void clear_collision_handlers(lua_State *state) noexcept;

/// Dispatches registered and legacy global collision callbacks. Each pair
/// carries the generation-bearing identities recorded at collision time;
/// every handler for a pair receives those same snapshotted identities, so
/// a handler that destroys a participant (and a spawn that recycles its
/// index) can never retarget the event for the handlers that follow — a
/// stale participant pushes as nil instead.
void dispatch_collision_handlers(lua_State *state,
                                 const core::Entity *pairData,
                                 std::size_t pairCount,
                                 PushEntityHandleFn pushEntityHandle) noexcept;

/// Lua binding: engine.on_trigger_handler(callback). The callback receives
/// (trigger, other, phase) with phase "enter" or "exit". Returns the
/// handler's id, or nil and a reason, as on_collision_handler does.
int lua_engine_on_trigger_register(lua_State *state) noexcept;
/// Lua binding: engine.remove_trigger_handler(handler_id).
int lua_engine_remove_trigger_handler(lua_State *state) noexcept;

/// Releases all registered Lua trigger callback refs.
void clear_trigger_handlers(lua_State *state) noexcept;

/// Dispatches each trigger event to every registered trigger handler in
/// id order. Events carry the identities recorded when they happened, as
/// collision pairs do: a participant no longer alive pushes as nil.
void dispatch_trigger_handlers(lua_State *state, const core::Entity *pairData,
                               const std::uint8_t *entered,
                               std::size_t eventCount,
                               PushEntityHandleFn pushEntityHandle) noexcept;

} // namespace engine::scripting
