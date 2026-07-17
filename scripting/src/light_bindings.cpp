// Implements light Lua bindings (directional, point, and spot light
// components)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "light_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "lua_state.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"

namespace engine::scripting {

namespace {

int lua_engine_add_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *typeStr =
      lua_isstring(state, 2) ? lua_tostring(state, 2) : nullptr;
  runtime::LightComponent light{};
  if ((typeStr != nullptr) && (std::strcmp(typeStr, "point") == 0)) {
    light.type = runtime::LightType::Point;
  } else {
    light.type = runtime::LightType::Directional;
  }
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine remove light.
int lua_engine_remove_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_light_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine has light.
int lua_engine_has_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().world->has_light_component(entity) ? 1 : 0);
  return 1;
}

/// Handles lua engine set light color.
int lua_engine_set_light_color(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 color{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &color)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.color = color;
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get light color.
int lua_engine_get_light_color(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(light.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(light.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(light.color.z));
  return 3;
}

/// Handles lua engine set light intensity.
int lua_engine_set_light_intensity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.intensity = static_cast<float>(lua_tonumber(state, 2));
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get light intensity.
int lua_engine_get_light_intensity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(light.intensity));
  return 1;
}

/// Handles lua engine set light direction.
int lua_engine_set_light_direction(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 dir{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &dir)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.direction = dir;
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- PointLightComponent bindings ---

// Lua: engine.add_point_light(entity, r, g, b, intensity, radius) → boolean
static int lua_engine_add_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 6) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::PointLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 5));
  comp.radius = static_cast<float>(luaL_checknumber(state, 6));
  const bool ok = apply_or_queue_point_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.get_point_light(entity) → r, g, b, intensity, radius or nil
static int lua_engine_get_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::PointLightComponent comp{};
  if (!runtime_binding().world->get_point_light_component(entity, &comp)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.z));
  lua_pushnumber(state, static_cast<lua_Number>(comp.intensity));
  lua_pushnumber(state, static_cast<lua_Number>(comp.radius));
  return 5;
}

// Lua: engine.set_point_light(entity, r, g, b, intensity, radius) → boolean
static int lua_engine_set_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 6) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!runtime_binding().world->has_point_light_component(entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::PointLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 5));
  comp.radius = static_cast<float>(luaL_checknumber(state, 6));
  const bool ok = apply_or_queue_point_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.remove_point_light(entity) → boolean
static int lua_engine_remove_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_point_light_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- SpotLightComponent bindings ---

// Lua: engine.add_spot_light(entity, r, g, b, dx, dy, dz, intensity, radius,
//                            innerAngle, outerAngle) → boolean
static int lua_engine_add_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 11) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::SpotLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.direction.x = static_cast<float>(luaL_checknumber(state, 5));
  comp.direction.y = static_cast<float>(luaL_checknumber(state, 6));
  comp.direction.z = static_cast<float>(luaL_checknumber(state, 7));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 8));
  comp.radius = static_cast<float>(luaL_checknumber(state, 9));
  comp.innerConeAngle = static_cast<float>(luaL_checknumber(state, 10));
  comp.outerConeAngle = static_cast<float>(luaL_checknumber(state, 11));
  const bool ok = apply_or_queue_spot_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.get_spot_light(entity) → r, g, b, dx, dy, dz, intensity, radius,
//                                      innerAngle, outerAngle or nil
static int lua_engine_get_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::SpotLightComponent comp{};
  if (!runtime_binding().world->get_spot_light_component(entity, &comp)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.z));
  lua_pushnumber(state, static_cast<lua_Number>(comp.direction.x));
  lua_pushnumber(state, static_cast<lua_Number>(comp.direction.y));
  lua_pushnumber(state, static_cast<lua_Number>(comp.direction.z));
  lua_pushnumber(state, static_cast<lua_Number>(comp.intensity));
  lua_pushnumber(state, static_cast<lua_Number>(comp.radius));
  lua_pushnumber(state, static_cast<lua_Number>(comp.innerConeAngle));
  lua_pushnumber(state, static_cast<lua_Number>(comp.outerConeAngle));
  return 10;
}

// Lua: engine.set_spot_light(entity, r, g, b, dx, dy, dz, intensity, radius,
//                            innerAngle, outerAngle) → boolean
static int lua_engine_set_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 11) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!runtime_binding().world->has_spot_light_component(entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::SpotLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.direction.x = static_cast<float>(luaL_checknumber(state, 5));
  comp.direction.y = static_cast<float>(luaL_checknumber(state, 6));
  comp.direction.z = static_cast<float>(luaL_checknumber(state, 7));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 8));
  comp.radius = static_cast<float>(luaL_checknumber(state, 9));
  comp.innerConeAngle = static_cast<float>(luaL_checknumber(state, 10));
  comp.outerConeAngle = static_cast<float>(luaL_checknumber(state, 11));
  const bool ok = apply_or_queue_spot_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.remove_spot_light(entity) → boolean
static int lua_engine_remove_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_spot_light_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_light_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_add_light);
  lua_setfield(state, -2, "add_light");
  lua_pushcfunction(state, &lua_engine_remove_light);
  lua_setfield(state, -2, "remove_light");
  lua_pushcfunction(state, &lua_engine_has_light);
  lua_setfield(state, -2, "has_light");
  lua_pushcfunction(state, &lua_engine_set_light_color);
  lua_setfield(state, -2, "set_light_color");
  lua_pushcfunction(state, &lua_engine_get_light_color);
  lua_setfield(state, -2, "get_light_color");
  lua_pushcfunction(state, &lua_engine_set_light_intensity);
  lua_setfield(state, -2, "set_light_intensity");
  lua_pushcfunction(state, &lua_engine_get_light_intensity);
  lua_setfield(state, -2, "get_light_intensity");
  lua_pushcfunction(state, &lua_engine_set_light_direction);
  lua_setfield(state, -2, "set_light_direction");
  lua_pushcfunction(state, &lua_engine_add_point_light);
  lua_setfield(state, -2, "add_point_light");
  lua_pushcfunction(state, &lua_engine_get_point_light);
  lua_setfield(state, -2, "get_point_light");
  lua_pushcfunction(state, &lua_engine_set_point_light);
  lua_setfield(state, -2, "set_point_light");
  lua_pushcfunction(state, &lua_engine_remove_point_light);
  lua_setfield(state, -2, "remove_point_light");
  lua_pushcfunction(state, &lua_engine_add_spot_light);
  lua_setfield(state, -2, "add_spot_light");
  lua_pushcfunction(state, &lua_engine_get_spot_light);
  lua_setfield(state, -2, "get_spot_light");
  lua_pushcfunction(state, &lua_engine_set_spot_light);
  lua_setfield(state, -2, "set_spot_light");
  lua_pushcfunction(state, &lua_engine_remove_spot_light);
  lua_setfield(state, -2, "remove_spot_light");
}

} // namespace engine::scripting
