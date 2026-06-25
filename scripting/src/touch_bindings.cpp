// Implements Lua touch and gesture bindings for the Engine scripting system.

#include "touch_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/touch_input.h"

namespace engine::scripting {
namespace {

lua_State *g_touchLuaState = nullptr;
int g_touchCallbackRef = LUA_NOREF;
int g_gestureCallbackRefs[4] = {LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};

void unregister_lua_touch_callback() noexcept;
void unregister_lua_gesture_callback(int index) noexcept;

/// Handles lua touch handler.
void lua_touch_handler(const core::TouchEvent &event,
                       void * /*userData*/) noexcept {
  if ((g_touchLuaState == nullptr) || (g_touchCallbackRef == LUA_NOREF)) {
    return;
  }

  lua_rawgeti(g_touchLuaState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  if (!lua_isfunction(g_touchLuaState, -1)) {
    lua_pop(g_touchLuaState, 1);
    return;
  }

  lua_newtable(g_touchLuaState);
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.touchId));
  lua_setfield(g_touchLuaState, -2, "id");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.x));
  lua_setfield(g_touchLuaState, -2, "x");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.y));
  lua_setfield(g_touchLuaState, -2, "y");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.pressure));
  lua_setfield(g_touchLuaState, -2, "pressure");
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.phase));
  lua_setfield(g_touchLuaState, -2, "phase");
  if (lua_pcall(g_touchLuaState, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(g_touchLuaState, -1);
    core::log_message(core::LogLevel::Error, "Scripting",
                      err ? err : "touch callback error");
    lua_pop(g_touchLuaState, 1);
  }
}

/// Handles lua gesture handler.
void lua_gesture_handler(const core::GestureEvent &event,
                         void * /*userData*/) noexcept {
  if (g_touchLuaState == nullptr) {
    return;
  }

  const int idx = static_cast<int>(event.type);
  if ((idx < 0) || (idx >= 4) || (g_gestureCallbackRefs[idx] == LUA_NOREF)) {
    return;
  }

  lua_rawgeti(g_touchLuaState, LUA_REGISTRYINDEX, g_gestureCallbackRefs[idx]);
  if (!lua_isfunction(g_touchLuaState, -1)) {
    lua_pop(g_touchLuaState, 1);
    return;
  }

  lua_newtable(g_touchLuaState);
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.type));
  lua_setfield(g_touchLuaState, -2, "type");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.tapX));
  lua_setfield(g_touchLuaState, -2, "tap_x");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.tapY));
  lua_setfield(g_touchLuaState, -2, "tap_y");
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.tapCount));
  lua_setfield(g_touchLuaState, -2, "tap_count");
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.swipeDir));
  lua_setfield(g_touchLuaState, -2, "swipe_dir");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.swipeVelocity));
  lua_setfield(g_touchLuaState, -2, "swipe_velocity");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.pinchScale));
  lua_setfield(g_touchLuaState, -2, "pinch_scale");
  lua_pushnumber(g_touchLuaState,
                 static_cast<lua_Number>(event.rotationRadians));
  lua_setfield(g_touchLuaState, -2, "rotation");
  if (lua_pcall(g_touchLuaState, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(g_touchLuaState, -1);
    core::log_message(core::LogLevel::Error, "Scripting",
                      err ? err : "gesture callback error");
    lua_pop(g_touchLuaState, 1);
  }
}

core::GestureType gesture_type_from_index(int index) noexcept {
  return static_cast<core::GestureType>(index);
}

void unregister_lua_touch_callback() noexcept {
  while (core::unregister_touch_callback(&lua_touch_handler, nullptr)) {
  }
}

void unregister_lua_gesture_callback(int index) noexcept {
  if ((index < 0) || (index >= 4)) {
    return;
  }

  const core::GestureType type = gesture_type_from_index(index);
  while (
      core::unregister_gesture_callback(type, &lua_gesture_handler, nullptr)) {
  }
}

} // namespace

int lua_engine_on_touch(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  if ((g_touchLuaState != nullptr) && (g_touchCallbackRef != LUA_NOREF)) {
    luaL_unref(g_touchLuaState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  }
  unregister_lua_touch_callback();
  g_touchLuaState = state;
  lua_pushvalue(state, 1);
  g_touchCallbackRef = luaL_ref(state, LUA_REGISTRYINDEX);
  if (!core::register_touch_callback(&lua_touch_handler, nullptr)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_touchCallbackRef);
    g_touchCallbackRef = LUA_NOREF;
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, 1);
  return 1;
}

void clear_touch_gesture_callbacks(lua_State *fallbackState) noexcept {
  unregister_lua_touch_callback();

  lua_State *refState = g_touchLuaState;
  if (refState == nullptr) {
    refState = fallbackState;
  }

  if ((refState != nullptr) && (g_touchCallbackRef != LUA_NOREF)) {
    luaL_unref(refState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  }
  g_touchCallbackRef = LUA_NOREF;

  for (int i = 0; i < 4; ++i) {
    unregister_lua_gesture_callback(i);
    if ((refState != nullptr) && (g_gestureCallbackRefs[i] != LUA_NOREF)) {
      luaL_unref(refState, LUA_REGISTRYINDEX, g_gestureCallbackRefs[i]);
    }
    g_gestureCallbackRefs[i] = LUA_NOREF;
  }

  g_touchLuaState = nullptr;
}

int lua_engine_on_gesture(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isfunction(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *typeStr = lua_tostring(state, 1);
  int idx = -1;
  if (std::strcmp(typeStr, "tap") == 0) {
    idx = 0;
  } else if (std::strcmp(typeStr, "swipe") == 0) {
    idx = 1;
  } else if (std::strcmp(typeStr, "pinch") == 0) {
    idx = 2;
  } else if (std::strcmp(typeStr, "rotate") == 0) {
    idx = 3;
  }
  if (idx < 0) {
    lua_pushboolean(state, 0);
    return 1;
  }

  if ((g_touchLuaState != nullptr) &&
      (g_gestureCallbackRefs[idx] != LUA_NOREF)) {
    luaL_unref(g_touchLuaState, LUA_REGISTRYINDEX, g_gestureCallbackRefs[idx]);
  }
  unregister_lua_gesture_callback(idx);
  g_touchLuaState = state;
  lua_pushvalue(state, 2);
  g_gestureCallbackRefs[idx] = luaL_ref(state, LUA_REGISTRYINDEX);
  if (!core::register_gesture_callback(gesture_type_from_index(idx),
                                       &lua_gesture_handler, nullptr)) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_gestureCallbackRefs[idx]);
    g_gestureCallbackRefs[idx] = LUA_NOREF;
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_set_touch_mouse_emulation(lua_State *state) noexcept {
  const bool enabled = lua_toboolean(state, 1) != 0;
  core::set_touch_mouse_emulation(enabled);
  return 0;
}

} // namespace engine::scripting
