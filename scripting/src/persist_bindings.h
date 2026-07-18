// Declares private Lua persist table bindings for the scripting module.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: Lua engine.persist(key, value).
int lua_engine_persist(lua_State *state) noexcept;
/// Lua binding: Lua engine.restore(key).
int lua_engine_restore(lua_State *state) noexcept;

/// Releases the Lua persist table registry ref.
void clear_persist_bindings(lua_State *state) noexcept;

} // namespace engine::scripting
