// Declares mesh and material Lua bindings (mesh assignment, procedural
// shape spawning, PBR material fields)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#pragma once

struct lua_State;

namespace engine::scripting {

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_mesh_material_bindings(lua_State *state) noexcept;
/// Clears the default/builtin mesh asset ids (engine shutdown).
void reset_mesh_material_bindings() noexcept;

} // namespace engine::scripting
