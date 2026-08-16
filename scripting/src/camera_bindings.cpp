// Implements camera Lua bindings (active camera, camera manager stack,
// shake, spring arms)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "camera_bindings.h"

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

int lua_engine_set_camera_position(lua_State *state) noexcept {
  math::Vec3 pos{};
  if (!read_vec3_args(state, 1, &pos) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_position == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_position(pos.x, pos.y, pos.z);
  return 0;
}

int lua_engine_set_camera_target(lua_State *state) noexcept {
  math::Vec3 target{};
  if (!read_vec3_args(state, 1, &target) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_target == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_target(target.x, target.y, target.z);
  return 0;
}

int lua_engine_set_camera_up(lua_State *state) noexcept {
  math::Vec3 up{};
  if (!read_vec3_args(state, 1, &up) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_up == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_up(up.x, up.y, up.z);
  return 0;
}

int lua_engine_set_camera_fov(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_fov == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_fov(static_cast<float>(lua_tonumber(state, 1)));
  return 0;
}

// -- Camera Manager Lua bindings ------------------------------------------

// Engine.push_camera(entityIndex, posX,posY,posZ, tgtX,tgtY,tgtZ, priority
// [, blendSpeed])
int lua_engine_push_camera(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->push_camera_op == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float posX = static_cast<float>(luaL_checknumber(state, 2));
  const float posY = static_cast<float>(luaL_checknumber(state, 3));
  const float posZ = static_cast<float>(luaL_checknumber(state, 4));
  const float tgtX = static_cast<float>(luaL_checknumber(state, 5));
  const float tgtY = static_cast<float>(luaL_checknumber(state, 6));
  const float tgtZ = static_cast<float>(luaL_checknumber(state, 7));
  const float priority = static_cast<float>(luaL_checknumber(state, 8));
  float blendSpeed = 5.0F;
  if (lua_isnumber(state, 9)) {
    blendSpeed = static_cast<float>(lua_tonumber(state, 9));
  }
  const bool ok =
      runtime_binding().services->push_camera_op(runtime_binding().world, entity.index, posX, posY, posZ, tgtX,
                                 tgtY, tgtZ, priority, blendSpeed);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.pop_camera(entityIndex)
int lua_engine_pop_camera(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->pop_camera_op == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = runtime_binding().services->pop_camera_op(runtime_binding().world, entity.index);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_active_camera() -> posX,posY,posZ, tgtX,tgtY,tgtZ, fov | nil
int lua_engine_get_active_camera(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
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
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->camera_shake_op == nullptr)) {
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
  if (runtime_binding().world == nullptr) {
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
  arm.offset.x = static_cast<float>(luaL_checknumber(state, 3));
  arm.offset.y = static_cast<float>(luaL_checknumber(state, 4));
  arm.offset.z = static_cast<float>(luaL_checknumber(state, 5));
  if (lua_isnumber(state, 6)) {
    arm.lagSpeed = static_cast<float>(lua_tonumber(state, 6));
  }
  if (lua_isboolean(state, 7)) {
    arm.collisionEnabled = (lua_toboolean(state, 7) != 0);
  }
  const bool ok = runtime_binding().world->add_spring_arm(entity, arm);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_spring_arm(entityIndex) -> armLength, currentLength, offX, offY,
// offZ, lagSpeed | nil
int lua_engine_get_spring_arm(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::SpringArmComponent arm{};
  if (!runtime_binding().world->get_spring_arm(entity, &arm)) {
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

// -- Authored CameraComponent Lua bindings (issue #161) --------------------
// Pose always comes from the entity's Transform (never supplied here); these
// bindings only touch fov/near/far/priority/blendSpeed/active so behaviour
// scripts can enable/disable/select/blend authored cameras by stable entity
// reference. fovRadians matches engine.set_camera_fov's existing convention.

// Engine.add_camera_component(entityIndex, fovRadians, nearPlane, farPlane,
// priority [, blendSpeed] [, active]) -> bool
int lua_engine_add_camera_component(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
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
  const bool ok = runtime_binding().world->add_camera_component(entity, camera);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_camera_component(entityIndex) -> fovRadians, nearPlane,
// farPlane, priority, blendSpeed, active | nil
int lua_engine_get_camera_component(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::CameraComponent camera{};
  if (!runtime_binding().world->get_camera_component(entity, &camera)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<double>(camera.fovRadians));
  lua_pushnumber(state, static_cast<double>(camera.nearPlane));
  lua_pushnumber(state, static_cast<double>(camera.farPlane));
  lua_pushnumber(state, static_cast<double>(camera.priority));
  lua_pushnumber(state, static_cast<double>(camera.blendSpeed));
  lua_pushboolean(state, camera.active ? 1 : 0);
  return 6;
}

// Engine.remove_camera_component(entityIndex) -> bool
int lua_engine_remove_camera_component(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = runtime_binding().world->remove_camera_component(entity);
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
  if (runtime_binding().world == nullptr) {
    return false;
  }
  runtime::CameraComponent camera{};
  if (!runtime_binding().world->get_camera_component(entity, &camera)) {
    return false;
  }
  apply(camera);
  return runtime_binding().world->add_camera_component(entity, camera);
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
// active priority wins CameraManager's stack (matches engine.push_camera).
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
  lua_pushcfunction(state, &lua_engine_set_camera_position);
  lua_setfield(state, -2, "set_camera_position");
  lua_pushcfunction(state, &lua_engine_set_camera_target);
  lua_setfield(state, -2, "set_camera_target");
  lua_pushcfunction(state, &lua_engine_set_camera_up);
  lua_setfield(state, -2, "set_camera_up");
  lua_pushcfunction(state, &lua_engine_set_camera_fov);
  lua_setfield(state, -2, "set_camera_fov");
  lua_pushcfunction(state, &lua_engine_push_camera);
  lua_setfield(state, -2, "push_camera");
  lua_pushcfunction(state, &lua_engine_pop_camera);
  lua_setfield(state, -2, "pop_camera");
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
