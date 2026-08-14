// Declares Lua entity pool bindings for the Engine scripting system.

#pragma once

#include <cstddef>

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Registers entity pool functions on the engine Lua table.
void register_entity_pool_bindings(lua_State *state) noexcept;

/// Clears all Lua-created entity pool state.
void reset_entity_pool_bindings() noexcept;

/// Count of allocated Lua-created entity pool slots (test/production
/// introspection for #93b: a scene transition must drain this to zero).
std::size_t pool_slot_count() noexcept;

} // namespace engine::scripting
