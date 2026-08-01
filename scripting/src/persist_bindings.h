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

/// Lua binding: engine.save_data(table) -> bool. Serializes a flat table
/// (string keys; number/string/bool values) to the single JSON save slot.
int lua_engine_save_data(lua_State *state) noexcept;

/// Lua binding: engine.load_data() -> table | nil from the save slot.
int lua_engine_load_data(lua_State *state) noexcept;

} // namespace engine::scripting
