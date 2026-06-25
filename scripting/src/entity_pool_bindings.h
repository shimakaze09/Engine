// Declares Lua entity pool bindings for the Engine scripting system.

#pragma once

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Registers entity pool functions on the engine Lua table.
void register_entity_pool_bindings(lua_State *state) noexcept;

/// Clears all Lua-created entity pool state.
void reset_entity_pool_bindings() noexcept;

} // namespace engine::scripting
