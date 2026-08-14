// Declares private Lua timer bindings for the scripting module.

#pragma once

#include <cstddef>

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

/// Count of live Lua registry refs held by timer callbacks (test/production
/// introspection for #93a: a scene transition must drain this to zero).
std::size_t active_lua_timer_ref_count() noexcept;

} // namespace engine::scripting
