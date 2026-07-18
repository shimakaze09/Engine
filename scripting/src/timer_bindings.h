// Declares private Lua timer bindings for the scripting module.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: Lua engine.set_timeout(callback, seconds).
int lua_engine_set_timeout(lua_State *state) noexcept;
/// Lua binding: Lua engine.set_interval(callback, seconds).
int lua_engine_set_interval(lua_State *state) noexcept;
/// Lua binding: Lua engine.cancel_timer(timer_id).
int lua_engine_cancel_timer(lua_State *state) noexcept;

/// Releases Lua timer refs and clears the bound world's timers.
void clear_lua_timer_bindings(lua_State *fallbackState) noexcept;

/// Rewires restored Lua timers and advances the current world timer manager.
void tick_lua_timers(lua_State *state, float deltaSeconds) noexcept;

} // namespace engine::scripting
