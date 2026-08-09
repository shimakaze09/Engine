// Implements private Lua input binding registration helpers.

#include "input_bindings.h"

extern "C" {
#include "lua.h"
}

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"

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

/// Reads an integer argument that must fit the int range; false refuses.
bool read_int_arg(lua_State *state, int index, int *outValue) noexcept {
  int isInteger = 0;
  const lua_Integer value = lua_tointegerx(state, index, &isInteger);
  if ((isInteger == 0) || (value < INT_MIN) || (value > INT_MAX)) {
    return false;
  }
  *outValue = static_cast<int>(value);
  return true;
}

/// Logs one refused-registration diagnostic naming the binding and field.
void log_field_refused(const char *context, const char *field) noexcept {
  char message[128] = {};
  std::snprintf(message, sizeof(message),
                "%s: field '%s' missing, non-integer, or out of range; "
                "registration refused",
                context, field);
  core::log_message(core::LogLevel::Error, "Scripting", message);
}

/// Reads one integer field on the table at tableIndex; a missing field uses
/// fallback, a non-integer or out-of-range value refuses with a diagnostic.
bool read_bounded_field(lua_State *state, int tableIndex, const char *context,
                        const char *field, lua_Integer minValue,
                        lua_Integer maxValue, lua_Integer fallback,
                        lua_Integer *outValue) noexcept {
  lua_getfield(state, tableIndex, field);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    *outValue = fallback;
    return true;
  }
  int isInteger = 0;
  const lua_Integer value = lua_tointegerx(state, -1, &isInteger);
  lua_pop(state, 1);
  if ((isInteger == 0) || (value < minValue) || (value > maxValue)) {
    log_field_refused(context, field);
    return false;
  }
  *outValue = value;
  return true;
}

/// Reads one finite number field; missing keeps the current value.
bool read_finite_field(lua_State *state, int tableIndex, const char *context,
                       const char *field, float *inOutValue) noexcept {
  lua_getfield(state, tableIndex, field);
  if (lua_isnil(state, -1)) {
    lua_pop(state, 1);
    return true;
  }
  const bool isNumber = lua_isnumber(state, -1) != 0;
  const float value = static_cast<float>(lua_tonumber(state, -1));
  lua_pop(state, 1);
  if (!isNumber || !std::isfinite(value)) {
    log_field_refused(context, field);
    return false;
  }
  *inOutValue = value;
  return true;
}

/// Parses an InputBinding table, refusing out-of-range type/code values.
bool read_binding_table(lua_State *state, int tableIndex, const char *context,
                        core::InputBinding *outBinding) noexcept {
  lua_Integer type = 0;
  lua_Integer code = 0;
  constexpr auto kMaxType =
      static_cast<lua_Integer>(core::InputBindingType::GamepadAxis);
  if (!read_bounded_field(state, tableIndex, context, "type", 0, kMaxType, 0,
                          &type) ||
      !read_bounded_field(state, tableIndex, context, "code", -1, INT_MAX, 0,
                          &code) ||
      !read_finite_field(state, tableIndex, context, "axis_threshold",
                         &outBinding->axisThreshold) ||
      !read_finite_field(state, tableIndex, context, "axis_scale",
                         &outBinding->axisScale)) {
    return false;
  }
  outBinding->type = static_cast<core::InputBindingType>(type);
  outBinding->code = static_cast<int>(code);
  return true;
}

/// Parses an InputAxisSource table, refusing out-of-range type/key/index.
bool read_axis_source_table(lua_State *state, int tableIndex,
                            const char *context,
                            core::InputAxisSource *outSource) noexcept {
  lua_Integer type = 0;
  lua_Integer negativeKey = 0;
  lua_Integer positiveKey = 0;
  lua_Integer axisIndex = 0;
  constexpr auto kMaxType =
      static_cast<lua_Integer>(core::AxisSourceType::MouseDeltaY);
  if (!read_bounded_field(state, tableIndex, context, "type", 0, kMaxType, 0,
                          &type) ||
      !read_bounded_field(state, tableIndex, context, "negative_key", -1,
                          INT_MAX, 0, &negativeKey) ||
      !read_bounded_field(state, tableIndex, context, "positive_key", -1,
                          INT_MAX, 0, &positiveKey) ||
      !read_bounded_field(state, tableIndex, context, "axis_index", -1,
                          INT_MAX, 0, &axisIndex) ||
      !read_finite_field(state, tableIndex, context, "scale",
                         &outSource->scale) ||
      !read_finite_field(state, tableIndex, context, "dead_zone",
                         &outSource->deadZone)) {
    return false;
  }
  outSource->type = static_cast<core::AxisSourceType>(type);
  outSource->negativeKey = static_cast<int>(negativeKey);
  outSource->positiveKey = static_cast<int>(positiveKey);
  outSource->axisIndex = static_cast<int>(axisIndex);
  return true;
}

/// Lua binding: Lua engine.is_key_down(scancode).
int lua_engine_is_key_down(lua_State *state) noexcept {
  int scancode = 0;
  if (!read_int_arg(state, 1, &scancode)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::is_key_down(scancode) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.is_key_pressed(scancode).
int lua_engine_is_key_pressed(lua_State *state) noexcept {
  int scancode = 0;
  if (!read_int_arg(state, 1, &scancode)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::is_key_pressed(scancode) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.register_action(name, key[, mouse_button]).
int lua_engine_register_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 1);
  int key = 0;
  int mouseButton = -1;
  if (!read_int_arg(state, 2, &key) ||
      (!lua_isnoneornil(state, 3) && !read_int_arg(state, 3, &mouseButton))) {
    log_field_refused("register_action", "key/mouse_button");
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = core::register_action(name, key, mouseButton);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.register_axis(name, negative_key, positive_key).
int lua_engine_register_axis(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 1);
  int negativeKey = 0;
  int positiveKey = 0;
  if (!read_int_arg(state, 2, &negativeKey) ||
      !read_int_arg(state, 3, &positiveKey)) {
    log_field_refused("register_axis", "negative_key/positive_key");
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = core::register_axis(name, negativeKey, positiveKey);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.is_action_down(name).
int lua_engine_is_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_down(name) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.is_action_pressed(name).
int lua_engine_is_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_pressed(name) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.action_value(name).
int lua_engine_get_action_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::action_value(name)));
  return 1;
}

/// Lua binding: Lua engine.axis_value(name).
int lua_engine_get_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::axis_value(name)));
  return 1;
}

/// Lua binding: Lua engine.is_gamepad_connected().
int lua_engine_is_gamepad_connected(lua_State *state) noexcept {
  static_cast<void>(state);
  lua_pushboolean(state, core::is_gamepad_connected() ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.is_gamepad_button_down(button).
int lua_engine_is_gamepad_button_down(lua_State *state) noexcept {
  int button = 0;
  if (!read_int_arg(state, 1, &button)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::is_gamepad_button_down(button) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.gamepad_axis_value(axis[, deadzone]).
int lua_engine_gamepad_axis_value(lua_State *state) noexcept {
  int axis = 0;
  int deadzone = 8000;
  if (!read_int_arg(state, 1, &axis) ||
      (!lua_isnoneornil(state, 2) && !read_int_arg(state, 2, &deadzone))) {
    lua_pushnumber(state, 0.0);
    return 1;
  }
  lua_pushnumber(
      state, static_cast<lua_Number>(core::gamepad_axis_value(axis, deadzone)));
  return 1;
}

/// Lua binding: Lua engine.add_input_action(name, bindings).
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
      if (!read_binding_table(state, lua_gettop(state), "add_input_action",
                              &bindings[count])) {
        lua_pop(state, 1);
        lua_pushboolean(state, 0);
        return 1;
      }
      ++count;
    }
    lua_pop(state, 1);
  }
  const bool ok = core::add_input_action(name, bindings, count);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.add_input_axis(name, sources).
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
      if (!read_axis_source_table(state, lua_gettop(state), "add_input_axis",
                                  &sources[count])) {
        lua_pop(state, 1);
        lua_pushboolean(state, 0);
        return 1;
      }
      ++count;
    }
    lua_pop(state, 1);
  }
  const bool ok = core::add_input_axis(name, sources, count);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.is_mapped_action_down(name).
int lua_engine_is_mapped_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_mapped_action_down(name) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.is_mapped_action_pressed(name).
int lua_engine_is_mapped_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_mapped_action_pressed(name) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.mapped_axis_value(name).
int lua_engine_mapped_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::mapped_axis_value(name)));
  return 1;
}

/// Lua binding: Lua engine.rebind_action(name, binding_index, binding).
int lua_engine_rebind_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2) ||
      !lua_istable(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  int bindingIndexArg = 0;
  if (!read_int_arg(state, 2, &bindingIndexArg) || (bindingIndexArg < 0) ||
      (bindingIndexArg >=
       static_cast<int>(core::kMaxBindingsPerAction))) {
    log_field_refused("rebind_action", "binding_index");
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto bindingIdx = static_cast<std::uint32_t>(bindingIndexArg);
  core::InputBinding binding{};
  if (!read_binding_table(state, 3, "rebind_action", &binding)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = core::rebind_action(name, bindingIdx, binding);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Confines a script-supplied config name strictly under the save directory.
bool resolve_input_config_path(const char *name, char *out,
                               std::size_t capacity) noexcept {
  if (!core::vfs_path_is_jailed(name)) {
    return false;
  }
  char saveDir[512] = {};
  if (!core::platform_get_save_dir(saveDir, sizeof(saveDir))) {
    return false;
  }
  const int written = std::snprintf(out, capacity, "%s/%s", saveDir, name);
  return (written > 0) && (static_cast<std::size_t>(written) < capacity);
}

/// Picks the per-user default path or the sandboxed name; false refuses.
bool resolve_lua_config_path(const char *name, char *out,
                             std::size_t capacity) noexcept {
  if (name == nullptr) {
    return core::input_bindings_default_path(out, capacity);
  }
  if (!resolve_input_config_path(name, out, capacity)) {
    core::log_message(core::LogLevel::Error, "Scripting",
                      "input config path refused: must be a relative "
                      "name under the save directory");
    return false;
  }
  return true;
}

/// Lua binding: Lua engine.save_input_config([name]); defaults to the
/// per-user bindings file; a name resolves under the save directory.
int lua_engine_save_input_config(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  char path[512] = {};
  if (!resolve_lua_config_path(name, path, sizeof(path))) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::save_input_bindings(path) ? 1 : 0);
  return 1;
}

/// Lua binding: Lua engine.load_input_config([name]); defaults to the
/// per-user bindings file; a name resolves under the save directory.
int lua_engine_load_input_config(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  char path[512] = {};
  if (!resolve_lua_config_path(name, path, sizeof(path))) {
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
