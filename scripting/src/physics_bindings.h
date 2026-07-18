// Declares physics Lua bindings (colliders, materials, layers, gravity,
// queries, joints)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#pragma once

struct lua_State;

namespace engine::scripting {

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_physics_bindings(lua_State *state) noexcept;

} // namespace engine::scripting
