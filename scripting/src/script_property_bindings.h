// Declares the Lua binding that reads a script property for an entity.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: engine.get_property(entity, name).
int lua_engine_get_property(lua_State *state) noexcept;

/// Forgets which property problems were already reported, so a new run
/// reports them again.
void reset_script_property_reports() noexcept;

} // namespace engine::scripting
