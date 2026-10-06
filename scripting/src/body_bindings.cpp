// Implements transform and rigid-body Lua bindings (position/rotation/scale,
// velocities, mass, sleep state)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "body_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "engine/math/quat.h"
#include "entity_handle.h"
#include "lua_state.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/string_util.h"
#include "engine/math/quat.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

namespace {

constexpr math::Vec3 kDefaultGravity(0.0F, -9.8F, 0.0F);

constexpr float kMaxScriptAcceleration = 500.0F;

int lua_engine_get_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  // Read through any same-frame queued transform write so a script
  // that sets then immediately gets sees its own write, not stale
  // committed state.
  runtime::Transform transform{};
  if (!latest_transform(entity, &transform)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(transform.position.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform.position.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform.position.z));
  return 3;
}

int lua_engine_set_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 position{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) || !read_vec3_arg(state, &vectorArg, &position)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  static_cast<void>(latest_transform(entity, &transform));
  transform.position = position;

  const bool ok = apply_or_queue_transform(entity, transform, true);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.z));
  return 3;
}

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

/// True, with a warning, when a velocity write targets a Static body: a
/// static body has no motion, so the write could only be discarded.
bool refuse_static_motion(const runtime::RigidBody &rigidBody,
                          const char *apiName) noexcept {
  if (math::body_type(rigidBody) != math::BodyType::Static) {
    return false;
  }
  char message[128] = {};
  std::snprintf(message, sizeof(message),
                "%s: the body is static and cannot move; make it kinematic "
                "or dynamic with engine.set_body_type",
                apiName);
  core::log_message(core::LogLevel::Warning, "scripting", message);
  return true;
}

int lua_engine_set_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 velocity{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) || !read_vec3_arg(state, &vectorArg, &velocity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_velocity requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  if (refuse_static_motion(rigidBody, "set_velocity")) {
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.velocity = velocity;
  if ((velocity.x != 0.0F) || (velocity.y != 0.0F) ||
      (velocity.z != 0.0F)) {
    rigidBody.sleeping = false;
    rigidBody.sleepFrameCount = 0U;
  }

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Current world gravity via the runtime service, defaulting when unbound.
math::Vec3 current_world_gravity() noexcept {
  float gx = 0.0F;
  float gy = 0.0F;
  float gz = 0.0F;
  if ((runtime_binding().services != nullptr) &&
      (runtime_binding().services->get_gravity != nullptr) &&
      runtime_binding().services->get_gravity(runtime_binding().world, &gx,
                                              &gy, &gz)) {
    return math::Vec3(gx, gy, gz);
  }
  return kDefaultGravity;
}

/// Stores the additive acceleration term, waking the body when the term
/// changes; identical rewrites leave a sleeping body asleep.
void store_acceleration_and_wake(runtime::RigidBody *rigidBody,
                                 const math::Vec3 &acceleration) noexcept {
  if ((rigidBody->acceleration.x != acceleration.x) ||
      (rigidBody->acceleration.y != acceleration.y) ||
      (rigidBody->acceleration.z != acceleration.z)) {
    rigidBody->sleeping = false;
    rigidBody->sleepFrameCount = 0U;
  }
  rigidBody->acceleration = acceleration;
}

// engine.set_acceleration(entity, x, y, z) → bool
// Accepts total world acceleration and converts it to the runtime's
// additive term used by physics integration (the current world gravity is
// subtracted); a changed effective acceleration wakes the body, and like
// set_velocity the motion command returns the body to physics control.
int lua_engine_set_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 acceleration{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_arg(state, &vectorArg, &acceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_acceleration requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  store_acceleration_and_wake(
      &rigidBody,
      math::clamp(math::sub(acceleration, current_world_gravity()),
                  -kMaxScriptAcceleration, kMaxScriptAcceleration));

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_additional_acceleration(entity, x, y, z) → bool
// Stores the additive term directly (applied on top of gravity), with the
// same stable-envelope clamp, wake-on-change rule, and return of the body
// to physics control as set_acceleration.
int lua_engine_set_additional_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 additionalAcceleration{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_arg(state, &vectorArg, &additionalAcceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_additional_acceleration requires an existing "
                      "RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  store_acceleration_and_wake(
      &rigidBody, math::clamp(additionalAcceleration, -kMaxScriptAcceleration,
                              kMaxScriptAcceleration));

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.z));
  return 3;
}

int lua_engine_set_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 angVel{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) || !read_vec3_arg(state, &vectorArg, &angVel)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_angular_velocity requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  if (refuse_static_motion(rigidBody, "set_angular_velocity")) {
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.angularVelocity = angVel;
  // Match set_velocity: commanding motion on a sleeping body has to wake it
  // or the command is integrated only once the body happens to wake.
  if ((angVel.x != 0.0F) || (angVel.y != 0.0F) || (angVel.z != 0.0F)) {
    rigidBody.sleeping = false;
    rigidBody.sleepFrameCount = 0U;
  }

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_wake_body(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->wake_body != nullptr)) {
    runtime_binding().services->wake_body(runtime_binding().world, entity);
  }
  return 0;
}

int lua_engine_is_sleeping(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->is_sleeping == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().services->is_sleeping(runtime_binding().world, entity) ? 1 : 0);
  return 1;
}

int lua_engine_get_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Transform transform{};
  if (!latest_transform(entity, &transform)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform.rotation.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform.rotation.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform.rotation.z));
  lua_pushnumber(state, static_cast<lua_Number>(transform.rotation.w));
  return 4;
}

/// Pushes one axis of the entity's rotation, in the space get_rotation
/// reports (world space for an entity with no parent), or nil.
int push_rotation_axis(lua_State *state,
                       math::Vec3 (*axisOf)(const math::Quat &)) noexcept {
  runtime::Entity entity{};
  runtime::Transform transform{};
  if (!read_entity(state, 1, &entity) ||
      !latest_transform(entity, &transform)) {
    lua_pushnil(state);
    return 1;
  }
  const math::Vec3 axis = axisOf(math::normalize(transform.rotation));
  lua_pushnumber(state, static_cast<lua_Number>(axis.x));
  lua_pushnumber(state, static_cast<lua_Number>(axis.y));
  lua_pushnumber(state, static_cast<lua_Number>(axis.z));
  return 3;
}

// engine.get_forward(entity) -> x, y, z: the way the entity faces (-Z).
int lua_engine_get_forward(lua_State *state) noexcept {
  return push_rotation_axis(
      state, [](const math::Quat &q) noexcept { return math::forward(q); });
}

// engine.get_right(entity) -> x, y, z: the entity's +X.
int lua_engine_get_right(lua_State *state) noexcept {
  return push_rotation_axis(
      state, [](const math::Quat &q) noexcept { return math::right(q); });
}

// engine.get_up(entity) -> x, y, z: the entity's +Y.
int lua_engine_get_up(lua_State *state) noexcept {
  return push_rotation_axis(
      state, [](const math::Quat &q) noexcept { return math::up(q); });
}

int lua_engine_set_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Quat rotation{};
  int rotationArg = 2;
  if (!read_entity(state, 1, &entity) ||
      !read_quat_arg(state, &rotationArg, &rotation)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  static_cast<void>(latest_transform(entity, &transform));
  transform.rotation = rotation;

  const bool ok = apply_or_queue_transform(entity, transform, true);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// engine.look_at(entity, x, y, z): turns the entity so its forward (-Z,
/// the way a camera looks) points at the point, its +Y kept upright, as
/// Unity's Transform.LookAt and Godot's look_at do. The point is in the
/// space the entity's position is in: world space for an entity with no
/// parent. False, changing nothing, for a point at the entity's own
/// position or straight above or below it, where no one rotation is meant.
int lua_engine_look_at(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 point{};
  int pointArg = 2;
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_arg(state, &pointArg, &point)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Transform transform{};
  math::Quat rotation{};
  if (!latest_transform(entity, &transform) ||
      !math::look_rotation(math::sub(point, transform.position),
                           math::Vec3(0.0F, 1.0F, 0.0F), &rotation)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  transform.rotation = rotation;
  const bool ok = apply_or_queue_transform(entity, transform, true);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Transform transform{};
  if (!latest_transform(entity, &transform)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform.scale.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform.scale.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform.scale.z));
  return 3;
}

int lua_engine_set_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 scale{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) || !read_vec3_arg(state, &vectorArg, &scale)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  static_cast<void>(latest_transform(entity, &transform));
  transform.scale = scale;

  const bool ok = apply_or_queue_transform(entity, transform, true);
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
  if (!latest_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.inverseMass));
  return 1;
}

int lua_engine_set_inverse_mass(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float previousInverseMass = rigidBody.inverseMass;
  rigidBody.inverseMass = static_cast<float>(lua_tonumber(state, 2));
  // An authored inverse inertia scales with the inverse mass for a fixed
  // shape, so a mass change keeps the body's rotational response
  // consistent; an automatic one is re-derived on apply.
  if ((previousInverseMass > 0.0F) && (rigidBody.inverseMass > 0.0F) &&
      std::isfinite(rigidBody.inverseMass) && rigidBody.inertiaAuthored) {
    rigidBody.inverseInertia = math::mul(
        rigidBody.inverseInertia, rigidBody.inverseMass / previousInverseMass);
  }
  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- RigidBody: body type ---

// Lua names of the BodyType values, indexed by the enumeration.
constexpr const char *kBodyTypeNames[math::kBodyTypeCount] = {
    "dynamic", "kinematic", "static"};

// engine.get_body_type(entity) -> "dynamic" | "kinematic" | "static" | nil
int lua_engine_get_body_type(lua_State *state) noexcept {
  runtime::Entity entity{};
  runtime::RigidBody rigidBody{};
  if (!read_entity(state, 1, &entity) ||
      !latest_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushstring(
      state,
      kBodyTypeNames[static_cast<std::uint32_t>(math::body_type(rigidBody))]);
  return 1;
}

// engine.set_body_type(entity, "dynamic" | "kinematic" | "static") -> bool.
// The authored mass and inertia are kept, so switching back restores
// them. A body made static loses its velocities; a dynamic one wakes.
int lua_engine_set_body_type(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || (lua_type(state, 2) != LUA_TSTRING)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_body_type expects an entity and \"dynamic\", "
                      "\"kinematic\" or \"static\"");
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 2);
  std::uint32_t type = math::kBodyTypeCount;
  for (std::uint32_t i = 0U; i < math::kBodyTypeCount; ++i) {
    if (std::strcmp(name, kBodyTypeNames[i]) == 0) {
      type = i;
    }
  }
  if (type == math::kBodyTypeCount) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "set_body_type: unknown body type '%.32s'; expected "
                  "dynamic, kinematic or static",
                  name);
    core::log_message(core::LogLevel::Warning, "scripting", message);
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_body_type requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.bodyType = type;
  rigidBody.sleeping = false;
  rigidBody.sleepFrameCount = 0U;
  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- Transform hierarchy: parent/children ---

// engine.set_parent(child, parent|nil) → bool
// Reparenting must not steal movement authority from physics or scripts,
// so the transform write releases authority to None.
int lua_engine_set_parent(lua_State *state) noexcept {
  runtime::Entity child{};
  if (!read_entity(state, 1, &child)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::PersistentId parentId = runtime::kInvalidPersistentId;
  if (!lua_isnoneornil(state, 2)) {
    runtime::Entity parent{};
    if (!read_entity(state, 2, &parent) || (parent == child)) {
      lua_pushboolean(state, 0);
      return 1;
    }
    parentId = runtime_binding().services->persistent_id(
        runtime_binding().world, parent);
    if (parentId == runtime::kInvalidPersistentId) {
      lua_pushboolean(state, 0);
      return 1;
    }
  }

  runtime::Transform transform{};
  if (!latest_transform(child, &transform)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_parent requires the child to have a Transform");
    lua_pushboolean(state, 0);
    return 1;
  }
  transform.parentId = parentId;

  const bool ok = apply_or_queue_transform(child, transform, false);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_parent(lua_State *state) noexcept {
  runtime::Entity child{};
  if (!read_entity(state, 1, &child)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::Transform transform{};
  if (!latest_transform(child, &transform) ||
      (transform.parentId == runtime::kInvalidPersistentId)) {
    lua_pushnil(state);
    return 1;
  }

  push_entity_handle(state,
                     runtime_binding().services->find_entity_by_persistent_id(
                         runtime_binding().world, transform.parentId));
  return 1;
}

/// Bridge visitor state for get_children: the table under construction.
struct ChildTableFill final {
  lua_State *state = nullptr;
  int childCount = 0;
};

/// Appends one child's handle to the table being filled.
void child_table_visit(core::Entity child, void *context) noexcept {
  auto *fill = static_cast<ChildTableFill *>(context);
  ++fill->childCount;
  push_entity_handle(fill->state, child);
  lua_rawseti(fill->state, -2, fill->childCount);
}

int lua_engine_get_children(lua_State *state) noexcept {
  runtime::Entity parent{};
  if (!read_entity(state, 1, &parent)) {
    lua_pushnil(state);
    return 1;
  }

  lua_newtable(state);
  // The World's child index answers in O(children), not O(transforms),
  // in child-link order.
  ChildTableFill fill{};
  fill.state = state;
  runtime_binding().services->for_each_child(runtime_binding().world, parent,
                                             &child_table_visit, &fill);
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
  lua_pushcfunction(state, &lua_engine_get_forward);
  lua_setfield(state, -2, "get_forward");
  lua_pushcfunction(state, &lua_engine_get_right);
  lua_setfield(state, -2, "get_right");
  lua_pushcfunction(state, &lua_engine_get_up);
  lua_setfield(state, -2, "get_up");
  lua_pushcfunction(state, &lua_engine_set_rotation);
  lua_setfield(state, -2, "set_rotation");
  lua_pushcfunction(state, &lua_engine_look_at);
  lua_setfield(state, -2, "look_at");
  lua_pushcfunction(state, &lua_engine_get_scale);
  lua_setfield(state, -2, "get_scale");
  lua_pushcfunction(state, &lua_engine_set_scale);
  lua_setfield(state, -2, "set_scale");
  lua_pushcfunction(state, &lua_engine_get_inverse_mass);
  lua_setfield(state, -2, "get_inverse_mass");
  lua_pushcfunction(state, &lua_engine_set_inverse_mass);
  lua_setfield(state, -2, "set_inverse_mass");
  lua_pushcfunction(state, &lua_engine_set_parent);
  lua_setfield(state, -2, "set_parent");
  lua_pushcfunction(state, &lua_engine_get_parent);
  lua_setfield(state, -2, "get_parent");
  lua_pushcfunction(state, &lua_engine_get_children);
  lua_setfield(state, -2, "get_children");
  lua_pushcfunction(state, &lua_engine_get_body_type);
  lua_setfield(state, -2, "get_body_type");
  lua_pushcfunction(state, &lua_engine_set_body_type);
  lua_setfield(state, -2, "set_body_type");
}

} // namespace engine::scripting
