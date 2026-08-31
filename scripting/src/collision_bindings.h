// Declares private Lua collision callback bindings for the scripting module.

#pragma once

#include <cstddef>

#include "engine/core/entity.h"

struct lua_State;

namespace engine::scripting {

using PushEntityHandleFn = void (*)(lua_State *state,
                                    core::Entity entity) noexcept;

/// Lua binding: Lua engine.on_collision_register(callback).
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

} // namespace engine::scripting
