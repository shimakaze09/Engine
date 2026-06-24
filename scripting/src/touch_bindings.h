// Declares Lua touch and gesture bindings for the Engine scripting system.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Handles Lua engine.on_touch(callback).
int lua_engine_on_touch(lua_State *state) noexcept;
/// Handles Lua engine.on_gesture(type, callback).
int lua_engine_on_gesture(lua_State *state) noexcept;
/// Handles Lua engine.set_touch_mouse_emulation(enabled).
int lua_engine_set_touch_mouse_emulation(lua_State *state) noexcept;

/// Releases Lua registry refs and unregisters touch/gesture callbacks.
void clear_touch_gesture_callbacks(lua_State *fallbackState) noexcept;

} // namespace engine::scripting
