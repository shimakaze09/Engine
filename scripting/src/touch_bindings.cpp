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

lua_State *g_touchMainState = nullptr;
int g_touchCallbackRef = LUA_NOREF;
int g_gestureCallbackRefs[4] = {LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};

/// Resolves the stable main thread that owns the shared Lua registry.
lua_State *resolve_main_state(lua_State *state) noexcept {
  if (state == nullptr) {
    return nullptr;
  }

  lua_rawgeti(state, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
  lua_State *mainState = lua_tothread(state, -1);
  lua_pop(state, 1);
  return mainState;
}

/// Reports whether any native touch hook still owns a Lua registry reference.
bool has_callback_refs() noexcept {
  if (g_touchCallbackRef != LUA_NOREF) {
    return true;
  }
  for (const int ref : g_gestureCallbackRefs) {
    if (ref != LUA_NOREF) {
      return true;
    }
  }
  return false;
}

/// Drops the cached main thread once no callback can dispatch into Lua.
void clear_main_state_if_unused() noexcept {
  if (!has_callback_refs()) {
    g_touchMainState = nullptr;
  }
}

void unregister_lua_touch_callback() noexcept;
void unregister_lua_gesture_callback(int index) noexcept;

void lua_touch_handler(const core::TouchEvent &event,
                       void * /*userData*/) noexcept {
  if ((g_touchMainState == nullptr) || (g_touchCallbackRef == LUA_NOREF)) {
    return;
  }

  lua_rawgeti(g_touchMainState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  if (!lua_isfunction(g_touchMainState, -1)) {
    lua_pop(g_touchMainState, 1);
    return;
  }

  lua_newtable(g_touchMainState);
  lua_pushinteger(g_touchMainState, static_cast<lua_Integer>(event.touchId));
  lua_setfield(g_touchMainState, -2, "id");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.x));
  lua_setfield(g_touchMainState, -2, "x");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.y));
  lua_setfield(g_touchMainState, -2, "y");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.pressure));
  lua_setfield(g_touchMainState, -2, "pressure");
  lua_pushinteger(g_touchMainState, static_cast<lua_Integer>(event.phase));
  lua_setfield(g_touchMainState, -2, "phase");
  if (lua_pcall(g_touchMainState, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(g_touchMainState, -1);
    core::log_message(core::LogLevel::Error, "Scripting",
                      err ? err : "touch callback error");
    lua_pop(g_touchMainState, 1);
  }
}

void lua_gesture_handler(const core::GestureEvent &event,
                         void * /*userData*/) noexcept {
  if (g_touchMainState == nullptr) {
    return;
  }

  const int idx = static_cast<int>(event.type);
  if ((idx < 0) || (idx >= 4) || (g_gestureCallbackRefs[idx] == LUA_NOREF)) {
    return;
  }

  lua_rawgeti(g_touchMainState, LUA_REGISTRYINDEX,
              g_gestureCallbackRefs[idx]);
  if (!lua_isfunction(g_touchMainState, -1)) {
    lua_pop(g_touchMainState, 1);
    return;
  }

  lua_newtable(g_touchMainState);
  lua_pushinteger(g_touchMainState, static_cast<lua_Integer>(event.type));
  lua_setfield(g_touchMainState, -2, "type");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.tapX));
  lua_setfield(g_touchMainState, -2, "tap_x");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.tapY));
  lua_setfield(g_touchMainState, -2, "tap_y");
  lua_pushinteger(g_touchMainState, static_cast<lua_Integer>(event.tapCount));
  lua_setfield(g_touchMainState, -2, "tap_count");
  lua_pushinteger(g_touchMainState, static_cast<lua_Integer>(event.swipeDir));
  lua_setfield(g_touchMainState, -2, "swipe_dir");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.swipeVelocity));
  lua_setfield(g_touchMainState, -2, "swipe_velocity");
  lua_pushnumber(g_touchMainState, static_cast<lua_Number>(event.pinchScale));
  lua_setfield(g_touchMainState, -2, "pinch_scale");
  lua_pushnumber(g_touchMainState,
                 static_cast<lua_Number>(event.rotationRadians));
  lua_setfield(g_touchMainState, -2, "rotation");
  if (lua_pcall(g_touchMainState, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(g_touchMainState, -1);
    core::log_message(core::LogLevel::Error, "Scripting",
                      err ? err : "gesture callback error");
    lua_pop(g_touchMainState, 1);
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

  lua_State *mainState = resolve_main_state(state);
  if (mainState == nullptr) {
    core::log_message(core::LogLevel::Error, "Scripting",
                      "failed to resolve main state for touch callback");
    lua_pushboolean(state, 0);
    return 1;
  }

  if ((g_touchMainState != nullptr) && (g_touchCallbackRef != LUA_NOREF)) {
    luaL_unref(g_touchMainState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  }
  unregister_lua_touch_callback();
  g_touchMainState = mainState;
  lua_pushvalue(state, 1);
  g_touchCallbackRef = luaL_ref(state, LUA_REGISTRYINDEX);
  if (!core::register_touch_callback(&lua_touch_handler, nullptr)) {
    luaL_unref(mainState, LUA_REGISTRYINDEX, g_touchCallbackRef);
    g_touchCallbackRef = LUA_NOREF;
    clear_main_state_if_unused();
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, 1);
  return 1;
}

void clear_touch_gesture_callbacks(lua_State *fallbackState) noexcept {
  unregister_lua_touch_callback();

  lua_State *refState = g_touchMainState;
  if (refState == nullptr) {
    refState = resolve_main_state(fallbackState);
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

  g_touchMainState = nullptr;
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

  lua_State *mainState = resolve_main_state(state);
  if (mainState == nullptr) {
    core::log_message(core::LogLevel::Error, "Scripting",
                      "failed to resolve main state for gesture callback");
    lua_pushboolean(state, 0);
    return 1;
  }

  if ((g_touchMainState != nullptr) &&
      (g_gestureCallbackRefs[idx] != LUA_NOREF)) {
    luaL_unref(g_touchMainState, LUA_REGISTRYINDEX,
               g_gestureCallbackRefs[idx]);
  }
  unregister_lua_gesture_callback(idx);
  g_touchMainState = mainState;
  lua_pushvalue(state, 2);
  g_gestureCallbackRefs[idx] = luaL_ref(state, LUA_REGISTRYINDEX);
  if (!core::register_gesture_callback(gesture_type_from_index(idx),
                                       &lua_gesture_handler, nullptr)) {
    luaL_unref(mainState, LUA_REGISTRYINDEX, g_gestureCallbackRefs[idx]);
    g_gestureCallbackRefs[idx] = LUA_NOREF;
    clear_main_state_if_unused();
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
