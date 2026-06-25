// Declares Lua scene management bindings for the Engine scripting system.

#pragma once

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Registers scene save/load/new functions on the engine Lua table.
void register_scene_bindings(lua_State *state) noexcept;

/// Clears any pending scene operation requested from Lua.
void reset_scene_bindings() noexcept;

} // namespace engine::scripting
