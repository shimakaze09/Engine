// Declares the navigation Lua bindings: path queries on the scene's
// navigation meshes.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_navigation_bindings(lua_State *state) noexcept;

} // namespace engine::scripting
