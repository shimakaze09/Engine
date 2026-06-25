// Declares private Lua input binding registration helpers.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Registers keyboard, action, gamepad, and input-map Lua bindings.
void register_input_bindings(lua_State *state) noexcept;

} // namespace engine::scripting
