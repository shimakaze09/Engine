// Implements private Lua input binding registration helpers.

#include "input_bindings.h"

extern "C" {
#include "lua.h"
}

#include <cstdint>

#include "engine/core/input.h"
#include "engine/core/input_map.h"

namespace engine::scripting {
namespace {

/// Registers one C function on the existing engine table.
void set_engine_function(lua_State *state, const char *name,
                         lua_CFunction function) noexcept {
  lua_pushcfunction(state, function);
  lua_setfield(state, -2, name);
}

/// Registers one integer constant on the existing engine table.
void set_engine_integer(lua_State *state, const char *name,
                        int value) noexcept {
  lua_pushinteger(state, value);
  lua_setfield(state, -2, name);
}

/// Handles Lua engine.is_key_down(scancode).
int lua_engine_is_key_down(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int scancode = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_key_down(scancode) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.is_key_pressed(scancode).
int lua_engine_is_key_pressed(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int scancode = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_key_pressed(scancode) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.register_action(name, key[, mouse_button]).
int lua_engine_register_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 1);
  const int key = static_cast<int>(lua_tointeger(state, 2));
  const int mouseButton =
      lua_isnumber(state, 3) ? static_cast<int>(lua_tointeger(state, 3)) : -1;
  const bool ok = core::register_action(name, key, mouseButton);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles Lua engine.register_axis(name, negative_key, positive_key).
int lua_engine_register_axis(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 1);
  const int negativeKey = static_cast<int>(lua_tointeger(state, 2));
  const int positiveKey = static_cast<int>(lua_tointeger(state, 3));
  const bool ok = core::register_axis(name, negativeKey, positiveKey);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles Lua engine.is_action_down(name).
int lua_engine_is_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_down(name) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.is_action_pressed(name).
int lua_engine_is_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_pressed(name) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.action_value(name).
int lua_engine_get_action_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::action_value(name)));
  return 1;
}

/// Handles Lua engine.axis_value(name).
int lua_engine_get_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::axis_value(name)));
  return 1;
}

/// Handles Lua engine.is_gamepad_connected().
int lua_engine_is_gamepad_connected(lua_State *state) noexcept {
  static_cast<void>(state);
  lua_pushboolean(state, core::is_gamepad_connected() ? 1 : 0);
  return 1;
}

/// Handles Lua engine.is_gamepad_button_down(button).
int lua_engine_is_gamepad_button_down(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int button = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_gamepad_button_down(button) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.gamepad_axis_value(axis[, deadzone]).
int lua_engine_gamepad_axis_value(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushnumber(state, 0.0);
    return 1;
  }
  const int axis = static_cast<int>(lua_tointeger(state, 1));
  const int deadzone =
      lua_isnumber(state, 2) ? static_cast<int>(lua_tointeger(state, 2)) : 8000;
  lua_pushnumber(
      state, static_cast<lua_Number>(core::gamepad_axis_value(axis, deadzone)));
  return 1;
}

/// Handles Lua engine.add_input_action(name, bindings).
int lua_engine_add_input_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_istable(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  core::InputBinding bindings[core::kMaxBindingsPerAction]{};
  std::uint32_t count = 0U;
  const int tableLen = static_cast<int>(lua_rawlen(state, 2));
  for (int i = 1; i <= tableLen && count < core::kMaxBindingsPerAction; ++i) {
    lua_rawgeti(state, 2, i);
    if (lua_istable(state, -1)) {
      lua_getfield(state, -1, "type");
      bindings[count].type =
          static_cast<core::InputBindingType>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "code");
      bindings[count].code = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "axis_threshold");
      if (lua_isnumber(state, -1)) {
        bindings[count].axisThreshold =
            static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      lua_getfield(state, -1, "axis_scale");
      if (lua_isnumber(state, -1)) {
        bindings[count].axisScale = static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      ++count;
    }
    lua_pop(state, 1);
  }
  const bool ok = core::add_input_action(name, bindings, count);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles Lua engine.add_input_axis(name, sources).
int lua_engine_add_input_axis(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_istable(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  core::InputAxisSource sources[core::kMaxSourcesPerAxis]{};
  std::uint32_t count = 0U;
  const int tableLen = static_cast<int>(lua_rawlen(state, 2));
  for (int i = 1; i <= tableLen && count < core::kMaxSourcesPerAxis; ++i) {
    lua_rawgeti(state, 2, i);
    if (lua_istable(state, -1)) {
      lua_getfield(state, -1, "type");
      sources[count].type =
          static_cast<core::AxisSourceType>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "negative_key");
      sources[count].negativeKey = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "positive_key");
      sources[count].positiveKey = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "axis_index");
      sources[count].axisIndex = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "scale");
      if (lua_isnumber(state, -1)) {
        sources[count].scale = static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      lua_getfield(state, -1, "dead_zone");
      if (lua_isnumber(state, -1)) {
        sources[count].deadZone = static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      ++count;
    }
    lua_pop(state, 1);
  }
  const bool ok = core::add_input_axis(name, sources, count);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles Lua engine.is_mapped_action_down(name).
int lua_engine_is_mapped_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_mapped_action_down(name) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.is_mapped_action_pressed(name).
int lua_engine_is_mapped_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_mapped_action_pressed(name) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.mapped_axis_value(name).
int lua_engine_mapped_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::mapped_axis_value(name)));
  return 1;
}

/// Handles Lua engine.rebind_action(name, binding_index, binding).
int lua_engine_rebind_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2) ||
      !lua_istable(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  const auto bindingIdx = static_cast<std::uint32_t>(lua_tointeger(state, 2));
  core::InputBinding binding{};
  lua_getfield(state, 3, "type");
  binding.type = static_cast<core::InputBindingType>(lua_tointeger(state, -1));
  lua_pop(state, 1);
  lua_getfield(state, 3, "code");
  binding.code = static_cast<int>(lua_tointeger(state, -1));
  lua_pop(state, 1);
  lua_getfield(state, 3, "axis_threshold");
  if (lua_isnumber(state, -1)) {
    binding.axisThreshold = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);
  lua_getfield(state, 3, "axis_scale");
  if (lua_isnumber(state, -1)) {
    binding.axisScale = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);
  const bool ok = core::rebind_action(name, bindingIdx, binding);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles Lua engine.save_input_config(path).
int lua_engine_save_input_config(lua_State *state) noexcept {
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::save_input_bindings(path) ? 1 : 0);
  return 1;
}

/// Handles Lua engine.load_input_config(path).
int lua_engine_load_input_config(lua_State *state) noexcept {
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::load_input_bindings(path) ? 1 : 0);
  return 1;
}

/// Registers key scancode constants on the existing engine table.
void register_key_constants(lua_State *state) noexcept {
  set_engine_integer(state, "KEY_A", core::kKey_A);
  set_engine_integer(state, "KEY_B", core::kKey_B);
  set_engine_integer(state, "KEY_C", core::kKey_C);
  set_engine_integer(state, "KEY_D", core::kKey_D);
  set_engine_integer(state, "KEY_E", core::kKey_E);
  set_engine_integer(state, "KEY_F", core::kKey_F);
  set_engine_integer(state, "KEY_G", core::kKey_G);
  set_engine_integer(state, "KEY_H", core::kKey_H);
  set_engine_integer(state, "KEY_I", core::kKey_I);
  set_engine_integer(state, "KEY_J", core::kKey_J);
  set_engine_integer(state, "KEY_K", core::kKey_K);
  set_engine_integer(state, "KEY_L", core::kKey_L);
  set_engine_integer(state, "KEY_M", core::kKey_M);
  set_engine_integer(state, "KEY_N", core::kKey_N);
  set_engine_integer(state, "KEY_O", core::kKey_O);
  set_engine_integer(state, "KEY_P", core::kKey_P);
  set_engine_integer(state, "KEY_Q", core::kKey_Q);
  set_engine_integer(state, "KEY_R", core::kKey_R);
  set_engine_integer(state, "KEY_S", core::kKey_S);
  set_engine_integer(state, "KEY_T", core::kKey_T);
  set_engine_integer(state, "KEY_U", core::kKey_U);
  set_engine_integer(state, "KEY_V", core::kKey_V);
  set_engine_integer(state, "KEY_W", core::kKey_W);
  set_engine_integer(state, "KEY_X", core::kKey_X);
  set_engine_integer(state, "KEY_Y", core::kKey_Y);
  set_engine_integer(state, "KEY_Z", core::kKey_Z);
  set_engine_integer(state, "KEY_0", core::kKey_0);
  set_engine_integer(state, "KEY_1", core::kKey_1);
  set_engine_integer(state, "KEY_2", core::kKey_2);
  set_engine_integer(state, "KEY_3", core::kKey_3);
  set_engine_integer(state, "KEY_4", core::kKey_4);
  set_engine_integer(state, "KEY_5", core::kKey_5);
  set_engine_integer(state, "KEY_6", core::kKey_6);
  set_engine_integer(state, "KEY_7", core::kKey_7);
  set_engine_integer(state, "KEY_8", core::kKey_8);
  set_engine_integer(state, "KEY_9", core::kKey_9);
  set_engine_integer(state, "KEY_SPACE", core::kKey_Space);
  set_engine_integer(state, "KEY_RETURN", core::kKey_Return);
  set_engine_integer(state, "KEY_ESCAPE", core::kKey_Escape);
  set_engine_integer(state, "KEY_UP", core::kKey_Up);
  set_engine_integer(state, "KEY_DOWN", core::kKey_Down);
  set_engine_integer(state, "KEY_LEFT", core::kKey_Left);
  set_engine_integer(state, "KEY_RIGHT", core::kKey_Right);
  set_engine_integer(state, "KEY_LSHIFT", core::kKey_LShift);
  set_engine_integer(state, "KEY_LCTRL", core::kKey_LCtrl);
  set_engine_integer(state, "KEY_LALT", core::kKey_LAlt);
}

} // namespace

void register_input_bindings(lua_State *state) noexcept {
  set_engine_function(state, "is_key_down", &lua_engine_is_key_down);
  set_engine_function(state, "is_key_pressed", &lua_engine_is_key_pressed);
  set_engine_function(state, "register_action", &lua_engine_register_action);
  set_engine_function(state, "register_axis", &lua_engine_register_axis);
  set_engine_function(state, "is_action_down", &lua_engine_is_action_down);
  set_engine_function(state, "is_action_pressed", &lua_engine_is_action_pressed);
  set_engine_function(state, "action_value", &lua_engine_get_action_value);
  set_engine_function(state, "axis_value", &lua_engine_get_axis_value);
  set_engine_function(state, "is_gamepad_connected",
                      &lua_engine_is_gamepad_connected);
  set_engine_function(state, "is_gamepad_button_down",
                      &lua_engine_is_gamepad_button_down);
  set_engine_function(state, "gamepad_axis_value",
                      &lua_engine_gamepad_axis_value);
  set_engine_function(state, "add_input_action", &lua_engine_add_input_action);
  set_engine_function(state, "add_input_axis", &lua_engine_add_input_axis);
  set_engine_function(state, "is_mapped_action_down",
                      &lua_engine_is_mapped_action_down);
  set_engine_function(state, "is_mapped_action_pressed",
                      &lua_engine_is_mapped_action_pressed);
  set_engine_function(state, "mapped_axis_value",
                      &lua_engine_mapped_axis_value);
  set_engine_function(state, "rebind_action", &lua_engine_rebind_action);
  set_engine_function(state, "save_input_config",
                      &lua_engine_save_input_config);
  set_engine_function(state, "load_input_config",
                      &lua_engine_load_input_config);

  register_key_constants(state);
}

} // namespace engine::scripting
