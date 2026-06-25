// Declares Lua and console cheat bindings for the Engine scripting system.

#pragma once

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Registers cheat status query functions on the engine Lua table.
void register_cheat_status_bindings(lua_State *state) noexcept;

/// Registers console cheat commands.
void register_cheat_commands() noexcept;

/// Resets cheat command state.
void reset_cheat_bindings() noexcept;

} // namespace engine::scripting
