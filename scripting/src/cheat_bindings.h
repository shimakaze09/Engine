// Declares Lua and console cheat bindings for the Engine scripting system.

#pragma once

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Registers console cheat commands.
void register_cheat_commands() noexcept;

/// Resets cheat command state.
void reset_cheat_bindings() noexcept;

} // namespace engine::scripting
