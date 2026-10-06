// Implements camera Lua bindings for the scripting module: reading the
// camera that renders, shake, spring arms and Camera components. A script
// moves a camera only by moving an entity that has a Camera component;
// nothing here reaches a camera that is not one, or the editor's.

#include "camera_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "lua_state.h"
#include "reload_transaction.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/math/world_component_types.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

namespace {

// Engine.get_active_camera() -> posX,posY,posZ, tgtX,tgtY,tgtZ, fov | nil
int lua_engine_get_active_camera(lua_State *state) noexcept {
  if (!runtime_bound() ||
      (runtime_binding().services->get_active_camera_op == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  float posX = 0.0F;
  float posY = 0.0F;
  float posZ = 0.0F;
  float tgtX = 0.0F;
  float tgtY = 0.0F;
  float tgtZ = 0.0F;
  float fov = 0.0F;
  if (!runtime_binding().services->get_active_camera_op(runtime_binding().world, &posX, &posY, &posZ, &tgtX,
                                        &tgtY, &tgtZ, &fov)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<double>(posX));
  lua_pushnumber(state, static_cast<double>(posY));
  lua_pushnumber(state, static_cast<double>(posZ));
  lua_pushnumber(state, static_cast<double>(tgtX));
  lua_pushnumber(state, static_cast<double>(tgtY));
  lua_pushnumber(state, static_cast<double>(tgtZ));
  lua_pushnumber(state, static_cast<double>(fov));
  return 7;
}

// Engine.camera_shake(amplitude, frequency, duration [, decay])
int lua_engine_camera_shake(lua_State *state) noexcept {
  if (!runtime_bound() ||
      (runtime_binding().services->camera_shake_op == nullptr) ||
      reload_refuses("camera_shake")) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float amplitude = static_cast<float>(luaL_checknumber(state, 1));
  const float frequency = static_cast<float>(luaL_checknumber(state, 2));
  const float duration = static_cast<float>(luaL_checknumber(state, 3));
  float decay = 2.0F;
  if (lua_isnumber(state, 4)) {
    decay = static_cast<float>(lua_tonumber(state, 4));
  }
  const bool ok = runtime_binding().services->camera_shake_op(runtime_binding().world, amplitude, frequency,
                                              duration, decay);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// -- Spring Arm Lua bindings -----------------------------------------------

// Engine.add_spring_arm(entityIndex, armLength, offsetX, offsetY, offsetZ
// [, lagSpeed] [, collisionEnabled])
int lua_engine_add_spring_arm(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::SpringArmComponent arm{};
  arm.armLength = static_cast<float>(luaL_checknumber(state, 2));
  arm.currentLength = arm.armLength;
  int arg = 3;
  if (!read_vec3_arg(state, &arg, &arm.offset)) {
    return luaL_argerror(state, 3, "an offset: a vec3 or three numbers");
  }
  if (lua_isnumber(state, arg)) {
    arm.lagSpeed = static_cast<float>(lua_tonumber(state, arg));
  }
  if (lua_isboolean(state, arg + 1)) {
    arm.collisionEnabled = (lua_toboolean(state, arg + 1) != 0);
  }
  const bool ok = apply_or_queue_spring_arm(entity, arm);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_spring_arm(entityIndex) -> armLength, currentLength, offX, offY,
// offZ, lagSpeed | nil
int lua_engine_get_spring_arm(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::SpringArmComponent arm{};
  if (!latest_spring_arm(entity, &arm)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<double>(arm.armLength));
  lua_pushnumber(state, static_cast<double>(arm.currentLength));
  lua_pushnumber(state, static_cast<double>(arm.offset.x));
  lua_pushnumber(state, static_cast<double>(arm.offset.y));
  lua_pushnumber(state, static_cast<double>(arm.offset.z));
  lua_pushnumber(state, static_cast<double>(arm.lagSpeed));
  return 6;
}

// -- Authored CameraComponent Lua bindings --------------------
// Pose always comes from the entity's Transform (never supplied here); these
// bindings only touch fov/near/far/priority/blendSpeed/active so behaviour
// scripts can enable/disable/select/blend authored cameras by stable entity
// reference. fovRadians is in radians, as the component stores it.

// Engine.add_camera_component(entityIndex, fovRadians, nearPlane, farPlane,
// priority [, blendSpeed] [, active]) -> bool
/// The values a camera can render with, as the camera manager accepts
/// them: finite, a near plane in front and a far one beyond it, no
/// negative blend speed, a positive size when orthographic. A script's
/// camera is refused here, at the setter, so a bad value never reaches the
/// frame that would drop it.
bool camera_component_valid(const runtime::CameraComponent &camera) noexcept {
  if (!std::isfinite(camera.fovRadians) || !std::isfinite(camera.nearPlane) ||
      !std::isfinite(camera.farPlane) ||
      !std::isfinite(camera.orthographicSize) ||
      !std::isfinite(camera.blendSpeed) || !std::isfinite(camera.priority)) {
    return false;
  }
  return (camera.nearPlane > 0.0F) && (camera.farPlane > camera.nearPlane) &&
         (camera.blendSpeed >= 0.0F) &&
         math::camera_projection_known(camera.projection) &&
         (!math::projection_is_orthographic(camera.projection) ||
          (camera.orthographicSize > 0.0F));
}

int lua_engine_add_camera_component(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::CameraComponent camera{};
  camera.fovRadians = static_cast<float>(luaL_checknumber(state, 2));
  camera.nearPlane = static_cast<float>(luaL_checknumber(state, 3));
  camera.farPlane = static_cast<float>(luaL_checknumber(state, 4));
  camera.priority = static_cast<float>(luaL_checknumber(state, 5));
  if (lua_isnumber(state, 6)) {
    camera.blendSpeed = static_cast<float>(lua_tonumber(state, 6));
  }
  if (lua_isboolean(state, 7)) {
    camera.active = (lua_toboolean(state, 7) != 0);
  }
  // Optional projection kind + orthographic half-height; an unknown
  // kind string is rejected rather than silently treated as perspective.
  if (lua_isstring(state, 8)) {
    const char *kind = lua_tostring(state, 8);
    if (std::strcmp(kind, "orthographic") == 0) {
      camera.projection =
          static_cast<std::uint32_t>(runtime::CameraProjection::Orthographic);
    } else if (std::strcmp(kind, "perspective") != 0) {
      lua_pushboolean(state, 0);
      return 1;
    }
  }
  if (lua_isnumber(state, 9)) {
    camera.orthographicSize = static_cast<float>(lua_tonumber(state, 9));
  }
  const bool ok = camera_component_valid(camera) &&
                  apply_or_queue_camera_component(entity, camera);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_camera_component(entityIndex) -> fovRadians, nearPlane,
// farPlane, priority, blendSpeed, active, projection, orthographicSize | nil
int lua_engine_get_camera_component(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::CameraComponent camera{};
  if (!latest_camera_component(entity, &camera)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<double>(camera.fovRadians));
  lua_pushnumber(state, static_cast<double>(camera.nearPlane));
  lua_pushnumber(state, static_cast<double>(camera.farPlane));
  lua_pushnumber(state, static_cast<double>(camera.priority));
  lua_pushnumber(state, static_cast<double>(camera.blendSpeed));
  lua_pushboolean(state, camera.active ? 1 : 0);
  lua_pushstring(state, math::projection_is_orthographic(camera.projection)
                            ? "orthographic"
                            : "perspective");
  lua_pushnumber(state, static_cast<double>(camera.orthographicSize));
  return 8;
}

// Engine.remove_camera_component(entityIndex) -> bool
int lua_engine_remove_camera_component(lua_State *state) noexcept {
  if (!runtime_bound()) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_camera_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Shared get-modify-add body for the single-field CameraComponent setters
/// below: reads the entity's current component, applies `apply`, and writes
/// it back through add_camera_component (so the World's own Input-phase
/// gating and storage-full checks stay the single source of truth -- no
/// second, unchecked mutation path).
template <typename ApplyFn>
bool set_camera_component_field(runtime::Entity entity,
                                ApplyFn &&apply) noexcept {
  if (!runtime_bound()) {
    return false;
  }
  runtime::CameraComponent camera{};
  if (!latest_camera_component(entity, &camera)) {
    return false;
  }
  apply(camera);
  return camera_component_valid(camera) &&
         apply_or_queue_camera_component(entity, camera);
}

// Engine.set_camera_component_active(entityIndex, active) -> bool
int lua_engine_set_camera_component_active(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool active = (lua_toboolean(state, 2) != 0);
  const bool ok = set_camera_component_field(
      entity, [active](runtime::CameraComponent &c) noexcept {
        c.active = active;
      });
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.set_camera_component_priority(entityIndex, priority) -> bool
// The mechanism for selecting which authored camera is active: the highest
// active priority wins CameraManager's stack.
int lua_engine_set_camera_component_priority(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float priority = static_cast<float>(luaL_checknumber(state, 2));
  const bool ok = set_camera_component_field(
      entity, [priority](runtime::CameraComponent &c) noexcept {
        c.priority = priority;
      });
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.set_camera_component_blend_speed(entityIndex, blendSpeed) -> bool
int lua_engine_set_camera_component_blend_speed(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float blendSpeed = static_cast<float>(luaL_checknumber(state, 2));
  const bool ok = set_camera_component_field(
      entity, [blendSpeed](runtime::CameraComponent &c) noexcept {
        c.blendSpeed = blendSpeed;
      });
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_camera_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_get_active_camera);
  lua_setfield(state, -2, "get_active_camera");
  lua_pushcfunction(state, &lua_engine_camera_shake);
  lua_setfield(state, -2, "camera_shake");
  lua_pushcfunction(state, &lua_engine_add_spring_arm);
  lua_setfield(state, -2, "add_spring_arm");
  lua_pushcfunction(state, &lua_engine_get_spring_arm);
  lua_setfield(state, -2, "get_spring_arm");
  lua_pushcfunction(state, &lua_engine_add_camera_component);
  lua_setfield(state, -2, "add_camera_component");
  lua_pushcfunction(state, &lua_engine_get_camera_component);
  lua_setfield(state, -2, "get_camera_component");
  lua_pushcfunction(state, &lua_engine_remove_camera_component);
  lua_setfield(state, -2, "remove_camera_component");
  lua_pushcfunction(state, &lua_engine_set_camera_component_active);
  lua_setfield(state, -2, "set_camera_component_active");
  lua_pushcfunction(state, &lua_engine_set_camera_component_priority);
  lua_setfield(state, -2, "set_camera_component_priority");
  lua_pushcfunction(state, &lua_engine_set_camera_component_blend_speed);
  lua_setfield(state, -2, "set_camera_component_blend_speed");
}

} // namespace engine::scripting
