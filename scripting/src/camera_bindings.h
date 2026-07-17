// Declares camera Lua bindings (active camera, camera manager stack,
// shake, spring arms)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#pragma once

struct lua_State;

namespace engine::scripting {

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_camera_bindings(lua_State *state) noexcept;

} // namespace engine::scripting
