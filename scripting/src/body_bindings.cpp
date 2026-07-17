// Implements transform and rigid-body Lua bindings (position/rotation/scale,
// velocities, mass, sleep state)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "body_bindings.h"

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

/// Handles k default gravity.
constexpr math::Vec3 kDefaultGravity(0.0F, -9.8F, 0.0F);

constexpr float kMaxScriptAcceleration = 500.0F;

/// Handles lua engine get position.
int lua_engine_get_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  if (runtime_binding().services == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Transform *transform =
      runtime_binding().services->get_transform_read_ptr(runtime_binding().world, entity.index);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(transform->position.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->position.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->position.z));
  return 3;
}

/// Handles lua engine set position.
int lua_engine_set_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 position{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &position)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  if (runtime_binding().services != nullptr) {
    static_cast<void>(
        runtime_binding().services->get_transform_op(runtime_binding().world, entity.index, &transform));
  }
  transform.position = position;

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get velocity.
int lua_engine_get_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.z));
  return 3;
}

/// Handles lua engine add rigid body.
int lua_engine_add_rigid_body(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  if ((lua_gettop(state) >= 2) && !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (lua_isnumber(state, 2)) {
    rigidBody.inverseMass = static_cast<float>(lua_tonumber(state, 2));
  }

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine set velocity.
int lua_engine_set_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 velocity{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &velocity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_velocity requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.velocity = velocity;

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine set acceleration.
int lua_engine_set_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 acceleration{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &acceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_acceleration requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  // set_acceleration accepts total world acceleration; convert to the
  // runtime's additive term used by physics integration.
  rigidBody.acceleration =
      math::clamp(math::sub(acceleration, kDefaultGravity),
                  -kMaxScriptAcceleration, kMaxScriptAcceleration);

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine set additional acceleration.
int lua_engine_set_additional_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 additionalAcceleration{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &additionalAcceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_additional_acceleration requires an existing "
                      "RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.acceleration = additionalAcceleration;

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get angular velocity.
int lua_engine_get_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.z));
  return 3;
}

/// Handles lua engine set angular velocity.
int lua_engine_set_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 angVel{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &angVel)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_angular_velocity requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.angularVelocity = angVel;

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine wake body.
int lua_engine_wake_body(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    return 0;
  }
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->wake_body != nullptr)) {
    const auto idx = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->wake_body(runtime_binding().world, idx);
  }
  return 0;
}

/// Handles lua engine is sleeping.
int lua_engine_is_sleeping(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->is_sleeping == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto idx = static_cast<std::uint32_t>(lua_tointeger(state, 1));
  lua_pushboolean(state, runtime_binding().services->is_sleeping(runtime_binding().world, idx) ? 1 : 0);
  return 1;
}

int lua_engine_get_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::Transform *transform = runtime_binding().world->get_transform_read_ptr(entity);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.z));
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.w));
  return 4;
}

/// Handles lua engine set rotation.
int lua_engine_set_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2) || !lua_isnumber(state, 3) ||
      !lua_isnumber(state, 4) || !lua_isnumber(state, 5)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float qx = static_cast<float>(lua_tonumber(state, 2));
  const float qy = static_cast<float>(lua_tonumber(state, 3));
  const float qz = static_cast<float>(lua_tonumber(state, 4));
  const float qw = static_cast<float>(lua_tonumber(state, 5));

  runtime::Transform transform{};
  static_cast<void>(runtime_binding().world->get_transform(entity, &transform));
  transform.rotation = math::Quat(qx, qy, qz, qw);

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get scale.
int lua_engine_get_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::Transform *transform = runtime_binding().world->get_transform_read_ptr(entity);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.z));
  return 3;
}

/// Handles lua engine set scale.
int lua_engine_set_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 scale{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &scale)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  static_cast<void>(runtime_binding().world->get_transform(entity, &transform));
  transform.scale = scale;

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- RigidBody: inverse mass ---

int lua_engine_get_inverse_mass(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.inverseMass));
  return 1;
}

/// Handles lua engine set inverse mass.
int lua_engine_set_inverse_mass(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.inverseMass = static_cast<float>(lua_tonumber(state, 2));
  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- Collider: getters ---

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_body_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_get_position);
  lua_setfield(state, -2, "get_position");
  lua_pushcfunction(state, &lua_engine_set_position);
  lua_setfield(state, -2, "set_position");
  lua_pushcfunction(state, &lua_engine_get_velocity);
  lua_setfield(state, -2, "get_velocity");
  lua_pushcfunction(state, &lua_engine_add_rigid_body);
  lua_setfield(state, -2, "add_rigid_body");
  lua_pushcfunction(state, &lua_engine_set_velocity);
  lua_setfield(state, -2, "set_velocity");
  lua_pushcfunction(state, &lua_engine_set_acceleration);
  lua_setfield(state, -2, "set_acceleration");
  lua_pushcfunction(state, &lua_engine_set_additional_acceleration);
  lua_setfield(state, -2, "set_additional_acceleration");
  lua_pushcfunction(state, &lua_engine_get_angular_velocity);
  lua_setfield(state, -2, "get_angular_velocity");
  lua_pushcfunction(state, &lua_engine_set_angular_velocity);
  lua_setfield(state, -2, "set_angular_velocity");
  lua_pushcfunction(state, &lua_engine_wake_body);
  lua_setfield(state, -2, "wake_body");
  lua_pushcfunction(state, &lua_engine_is_sleeping);
  lua_setfield(state, -2, "is_sleeping");
  lua_pushcfunction(state, &lua_engine_get_rotation);
  lua_setfield(state, -2, "get_rotation");
  lua_pushcfunction(state, &lua_engine_set_rotation);
  lua_setfield(state, -2, "set_rotation");
  lua_pushcfunction(state, &lua_engine_get_scale);
  lua_setfield(state, -2, "get_scale");
  lua_pushcfunction(state, &lua_engine_set_scale);
  lua_setfield(state, -2, "set_scale");
  lua_pushcfunction(state, &lua_engine_get_inverse_mass);
  lua_setfield(state, -2, "get_inverse_mass");
  lua_pushcfunction(state, &lua_engine_set_inverse_mass);
  lua_setfield(state, -2, "set_inverse_mass");
}

} // namespace engine::scripting
