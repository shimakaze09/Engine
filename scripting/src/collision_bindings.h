// Declares private Lua collision callback bindings for the scripting module.

#pragma once

#include <cstddef>
#include <cstdint>

struct lua_State;

namespace engine::scripting {

using PushEntityHandleFromIndexFn =
    void (*)(lua_State *state, std::uint32_t entityIndex) noexcept;
using LogLuaErrorFn = void (*)(const char *context) noexcept;

/// Lua binding: Lua engine.on_collision_register(callback).
int lua_engine_on_collision_register(lua_State *state) noexcept;
/// Lua binding: Lua engine.remove_collision_handler(handler_id).
int lua_engine_remove_collision_handler(lua_State *state) noexcept;

/// Releases all registered Lua collision callback refs.
void clear_collision_handlers(lua_State *state) noexcept;

/// Dispatches registered and legacy global collision callbacks.
void dispatch_collision_handlers(
    lua_State *state, const std::uint32_t *pairData, std::size_t pairCount,
    PushEntityHandleFromIndexFn pushEntityHandleFromIndex,
    LogLuaErrorFn logLuaError) noexcept;

} // namespace engine::scripting
