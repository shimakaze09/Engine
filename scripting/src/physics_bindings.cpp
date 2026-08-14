// Implements physics Lua bindings (colliders, materials, layers, gravity,
// queries, joints)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "physics_bindings.h"

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

/// One captured pre-lock inverse inertia, generation-checked (issue #80).
struct LockRotationCapture final {
  core::Entity owner = core::kInvalidEntity;
  float inverseInertia = 1.0F;
};

constexpr std::size_t kMaxLockCaptures = ENGINE_MAX_ENTITIES + 1U;
LockRotationCapture g_lockRotationCaptures[kMaxLockCaptures]{};

// engine.add_capsule_collider(entity, half_height, radius) → bool
int lua_engine_add_capsule_collider(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2) || !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float halfHeight = static_cast<float>(lua_tonumber(state, 2));
  const float radius = static_cast<float>(lua_tonumber(state, 3));

  runtime::Collider collider{};
  collider.shape = runtime::ColliderShape::Capsule;
  collider.halfExtents = math::Vec3(radius, halfHeight, radius);

  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_add_collider(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 halfExtents{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &halfExtents)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Collider collider{};
  collider.halfExtents = halfExtents;

  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_restitution(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float value = static_cast<float>(lua_tonumber(state, 2));
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  collider.restitution = value;
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_friction(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2) || !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float staticF = static_cast<float>(lua_tonumber(state, 2));
  const float dynamicF = static_cast<float>(lua_tonumber(state, 3));
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  collider.staticFriction = staticF;
  collider.dynamicFriction = dynamicF;
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_lock_rotation(entity, locked) → bool
// Freezes the body's rotational response, capturing its inverse inertia;
// unlock restores exactly the captured value (owner decision, issue #80).
// Without a capture, unlock of a locked body falls back to the 1.0 default
// (pre-capture legacy) and unlock of an unlocked body leaves it unchanged.
int lua_engine_set_lock_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isboolean(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool locked = lua_toboolean(state, 2) != 0;

  runtime::RigidBody rigidBody{};
  if (!latest_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_lock_rotation requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  if (entity.index >= kMaxLockCaptures) {
    lua_pushboolean(state, 0);
    return 1;
  }
  LockRotationCapture &capture = g_lockRotationCaptures[entity.index];
  if (locked) {
    if (rigidBody.inverseInertia != 0.0F) {
      capture.owner = entity;
      capture.inverseInertia = rigidBody.inverseInertia;
    }
    rigidBody.inverseInertia = 0.0F;
    rigidBody.angularVelocity = math::Vec3(0.0F, 0.0F, 0.0F);
  } else if (capture.owner == entity) {
    rigidBody.inverseInertia = capture.inverseInertia;
    capture = LockRotationCapture{};
  } else if (rigidBody.inverseInertia == 0.0F) {
    rigidBody.inverseInertia = 1.0F;
  }

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.create_physics_material(static_friction, dynamic_friction,
//                                restitution, density) → table
int lua_engine_create_physics_material(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3)) {
    lua_pushnil(state);
    return 1;
  }
  lua_createtable(state, 0, 4);
  lua_pushnumber(state, lua_tonumber(state, 1));
  lua_setfield(state, -2, "static_friction");
  lua_pushnumber(state, lua_tonumber(state, 2));
  lua_setfield(state, -2, "dynamic_friction");
  lua_pushnumber(state, lua_tonumber(state, 3));
  lua_setfield(state, -2, "restitution");
  const float density = lua_isnumber(state, 4)
                            ? static_cast<float>(lua_tonumber(state, 4))
                            : 1.0F;
  lua_pushnumber(state, static_cast<lua_Number>(density));
  lua_setfield(state, -2, "density");
  return 1;
}

// engine.set_collider_material(entity, material_table) → bool
int lua_engine_set_collider_material(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_istable(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_getfield(state, 2, "static_friction");
  if (lua_isnumber(state, -1)) {
    collider.staticFriction = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  lua_getfield(state, 2, "dynamic_friction");
  if (lua_isnumber(state, -1)) {
    collider.dynamicFriction = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  lua_getfield(state, 2, "restitution");
  if (lua_isnumber(state, -1)) {
    collider.restitution = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  lua_getfield(state, 2, "density");
  if (lua_isnumber(state, -1)) {
    collider.density = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_collision_layer(entity, layer_bits) → bool
int lua_engine_set_collision_layer(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.collisionLayer = static_cast<std::uint32_t>(lua_tointeger(state, 2));
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_collision_mask(entity, mask_bits) → bool
int lua_engine_set_collision_mask(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.collisionMask = static_cast<std::uint32_t>(lua_tointeger(state, 2));
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_gravity(lua_State *state) noexcept {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  if (lua_isnumber(state, 1)) {
    x = static_cast<float>(lua_tonumber(state, 1));
  }
  if (lua_isnumber(state, 2)) {
    y = static_cast<float>(lua_tonumber(state, 2));
  }
  if (lua_isnumber(state, 3)) {
    z = static_cast<float>(lua_tonumber(state, 3));
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_gravity != nullptr)) {
    runtime_binding().services->set_gravity(runtime_binding().world, x, y, z);
  }
  return 0;
}

int lua_engine_get_gravity(lua_State *state) noexcept {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->get_gravity == nullptr) ||
      !runtime_binding().services->get_gravity(runtime_binding().world, &x, &y, &z)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(x));
  lua_pushnumber(state, static_cast<lua_Number>(y));
  lua_pushnumber(state, static_cast<lua_Number>(z));
  return 3;
}

int lua_engine_raycast(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7)) {
    lua_pushnil(state);
    return 1;
  }
  const float ox = static_cast<float>(lua_tonumber(state, 1));
  const float oy = static_cast<float>(lua_tonumber(state, 2));
  const float oz = static_cast<float>(lua_tonumber(state, 3));
  const float dx = static_cast<float>(lua_tonumber(state, 4));
  const float dy = static_cast<float>(lua_tonumber(state, 5));
  const float dz = static_cast<float>(lua_tonumber(state, 6));
  const float maxDist = static_cast<float>(lua_tonumber(state, 7));

  RuntimeRaycastHit hit{};
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->raycast == nullptr) ||
      !runtime_binding().services->raycast(runtime_binding().world, ox, oy, oz, dx, dy, dz, maxDist, &hit)) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, hit.entityIndex);
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

// engine.raycast_all(ox,oy,oz, dx,dy,dz, max_dist [, mask]) → table of hits
int lua_engine_raycast_all(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->raycast_all == nullptr)) {
    lua_newtable(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7)) {
    lua_newtable(state);
    return 1;
  }
  const float ox = static_cast<float>(lua_tonumber(state, 1));
  const float oy = static_cast<float>(lua_tonumber(state, 2));
  const float oz = static_cast<float>(lua_tonumber(state, 3));
  const float dx = static_cast<float>(lua_tonumber(state, 4));
  const float dy = static_cast<float>(lua_tonumber(state, 5));
  const float dz = static_cast<float>(lua_tonumber(state, 6));
  const float maxDist = static_cast<float>(lua_tonumber(state, 7));
  const std::uint32_t mask =
      lua_isnumber(state, 8)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 8))
          : 0xFFFFFFFFU;

  constexpr std::size_t kMaxHits = 32U;
  RuntimeRaycastHit hits[kMaxHits]{};
  const std::size_t count = runtime_binding().services->raycast_all(
      runtime_binding().world, ox, oy, oz, dx, dy, dz, maxDist, hits, kMaxHits, mask);

  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    lua_createtable(state, 0, 8);
    push_entity_handle_from_index(state, hits[i].entityIndex);
    lua_setfield(state, -2, "entity");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].distance));
    lua_setfield(state, -2, "distance");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].pointX));
    lua_setfield(state, -2, "px");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].pointY));
    lua_setfield(state, -2, "py");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].pointZ));
    lua_setfield(state, -2, "pz");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].normalX));
    lua_setfield(state, -2, "nx");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].normalY));
    lua_setfield(state, -2, "ny");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].normalZ));
    lua_setfield(state, -2, "nz");
    lua_rawseti(state, -2, static_cast<int>(i + 1U));
  }
  return 1;
}

// engine.overlap_sphere(cx,cy,cz, radius [, mask]) → table of entity indices
int lua_engine_overlap_sphere(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->overlap_sphere == nullptr)) {
    lua_newtable(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4)) {
    lua_newtable(state);
    return 1;
  }
  const float cx = static_cast<float>(lua_tonumber(state, 1));
  const float cy = static_cast<float>(lua_tonumber(state, 2));
  const float cz = static_cast<float>(lua_tonumber(state, 3));
  const float radius = static_cast<float>(lua_tonumber(state, 4));
  const std::uint32_t mask =
      lua_isnumber(state, 5)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 5))
          : 0xFFFFFFFFU;

  constexpr std::size_t kMaxResults = 64U;
  std::uint32_t indices[kMaxResults]{};
  const std::size_t count = runtime_binding().services->overlap_sphere(
      runtime_binding().world, cx, cy, cz, radius, indices, kMaxResults, mask);

  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    push_entity_handle_from_index(state, indices[i]);
    lua_rawseti(state, -2, static_cast<int>(i + 1U));
  }
  return 1;
}

// engine.overlap_box(cx,cy,cz, hx,hy,hz [, mask]) → table of entity indices
int lua_engine_overlap_box(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->overlap_box == nullptr)) {
    lua_newtable(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6)) {
    lua_newtable(state);
    return 1;
  }
  const float cx = static_cast<float>(lua_tonumber(state, 1));
  const float cy = static_cast<float>(lua_tonumber(state, 2));
  const float cz = static_cast<float>(lua_tonumber(state, 3));
  const float hx = static_cast<float>(lua_tonumber(state, 4));
  const float hy = static_cast<float>(lua_tonumber(state, 5));
  const float hz = static_cast<float>(lua_tonumber(state, 6));
  const std::uint32_t mask =
      lua_isnumber(state, 7)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 7))
          : 0xFFFFFFFFU;

  constexpr std::size_t kMaxResults = 64U;
  std::uint32_t indices[kMaxResults]{};
  const std::size_t count = runtime_binding().services->overlap_box(
      runtime_binding().world, cx, cy, cz, hx, hy, hz, indices, kMaxResults, mask);

  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    push_entity_handle_from_index(state, indices[i]);
    lua_rawseti(state, -2, static_cast<int>(i + 1U));
  }
  return 1;
}

/// Decodes the optional trailing skip-entity argument for a sweep binding.
/// Returns false when a present argument is not a live entity handle.
bool read_optional_skip_entity(lua_State *state, int index,
                               std::uint32_t *outSkipIndex) noexcept {
  *outSkipIndex = 0U;
  if (lua_isnoneornil(state, index)) {
    return true;
  }
  runtime::Entity skipEntity{};
  if (!read_entity(state, index, &skipEntity)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "sweep skip entity is invalid or stale");
    return false;
  }
  *outSkipIndex = skipEntity.index;
  return true;
}

// engine.sweep_sphere(ox,oy,oz, radius, dx,dy,dz, max_dist
//                     [, mask [, skip_entity]])
// skip_entity excludes that entity's colliders and any compound-body
// colliders it owns from the sweep.
int lua_engine_sweep_sphere(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->sweep_sphere == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7) || !lua_isnumber(state, 8)) {
    lua_pushnil(state);
    return 1;
  }
  const float ox = static_cast<float>(lua_tonumber(state, 1));
  const float oy = static_cast<float>(lua_tonumber(state, 2));
  const float oz = static_cast<float>(lua_tonumber(state, 3));
  const float radius = static_cast<float>(lua_tonumber(state, 4));
  const float dx = static_cast<float>(lua_tonumber(state, 5));
  const float dy = static_cast<float>(lua_tonumber(state, 6));
  const float dz = static_cast<float>(lua_tonumber(state, 7));
  const float maxDist = static_cast<float>(lua_tonumber(state, 8));
  const std::uint32_t mask =
      lua_isnumber(state, 9)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 9))
          : 0xFFFFFFFFU;
  std::uint32_t skipIndex = 0U;
  if (!read_optional_skip_entity(state, 10, &skipIndex)) {
    lua_pushnil(state);
    return 1;
  }

  RuntimeRaycastHit hit{};
  if (!runtime_binding().services->sweep_sphere(runtime_binding().world, ox, oy, oz, radius, dx, dy, dz,
                                maxDist, &hit, mask, skipIndex)) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, hit.entityIndex);
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

// engine.sweep_box(cx,cy,cz, hx,hy,hz, dx,dy,dz, max_dist
//                  [, mask [, skip_entity]])
// skip_entity excludes that entity's colliders and any compound-body
// colliders it owns from the sweep.
int lua_engine_sweep_box(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->sweep_box == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7) || !lua_isnumber(state, 8) ||
      !lua_isnumber(state, 9) || !lua_isnumber(state, 10)) {
    lua_pushnil(state);
    return 1;
  }
  const float cx = static_cast<float>(lua_tonumber(state, 1));
  const float cy = static_cast<float>(lua_tonumber(state, 2));
  const float cz = static_cast<float>(lua_tonumber(state, 3));
  const float hx = static_cast<float>(lua_tonumber(state, 4));
  const float hy = static_cast<float>(lua_tonumber(state, 5));
  const float hz = static_cast<float>(lua_tonumber(state, 6));
  const float dx = static_cast<float>(lua_tonumber(state, 7));
  const float dy = static_cast<float>(lua_tonumber(state, 8));
  const float dz = static_cast<float>(lua_tonumber(state, 9));
  const float maxDist = static_cast<float>(lua_tonumber(state, 10));
  const std::uint32_t mask =
      lua_isnumber(state, 11)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 11))
          : 0xFFFFFFFFU;
  std::uint32_t skipIndex = 0U;
  if (!read_optional_skip_entity(state, 12, &skipIndex)) {
    lua_pushnil(state);
    return 1;
  }

  RuntimeRaycastHit hit{};
  if (!runtime_binding().services->sweep_box(runtime_binding().world, cx, cy, cz, hx, hy, hz, dx, dy, dz,
                             maxDist, &hit, mask, skipIndex)) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, hit.entityIndex);
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

/// Pushes a joint constructor result: nil for the unified 0 failure
/// sentinel, the id otherwise (issue #100 contract).
int push_joint_result(lua_State *state, std::uint32_t id) noexcept {
  if (id == 0U) {
    lua_pushnil(state);
  } else {
    lua_pushinteger(state, static_cast<lua_Integer>(id));
  }
  return 1;
}

// engine.add_distance_joint(entityA, entityB [, distance]) → joint_id | nil
int lua_engine_add_distance_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_distance_joint == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  const float dist = lua_isnumber(state, 3)
                         ? static_cast<float>(lua_tonumber(state, 3))
                         : 1.0F;
  const std::uint32_t id = runtime_binding().services->add_distance_joint(
      runtime_binding().world, entityA.index, entityB.index, dist);
  return push_joint_result(state, id);
}

int lua_engine_remove_joint(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->remove_joint != nullptr)) {
    runtime_binding().services->remove_joint(
        runtime_binding().world, static_cast<std::uint32_t>(lua_tointeger(state, 1)));
  }
  return 0;
}

// engine.add_hinge_joint(entityA, entityB, pivotX, pivotY, pivotZ, axisX,
// axisY, axisZ) → joint_id | nil
int lua_engine_add_hinge_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_hinge_joint == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  const auto px = static_cast<float>(luaL_optnumber(state, 3, 0.0));
  const auto py = static_cast<float>(luaL_optnumber(state, 4, 0.0));
  const auto pz = static_cast<float>(luaL_optnumber(state, 5, 0.0));
  const auto ax = static_cast<float>(luaL_optnumber(state, 6, 0.0));
  const auto ay = static_cast<float>(luaL_optnumber(state, 7, 1.0));
  const auto az = static_cast<float>(luaL_optnumber(state, 8, 0.0));
  const std::uint32_t id = runtime_binding().services->add_hinge_joint(
      runtime_binding().world, entityA.index, entityB.index, px, py, pz, ax, ay, az);
  return push_joint_result(state, id);
}

// engine.add_ball_socket_joint(entityA, entityB, pivotX, pivotY, pivotZ)
// → joint_id | nil
int lua_engine_add_ball_socket_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) ||
      (runtime_binding().services->add_ball_socket_joint == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  const auto px = static_cast<float>(luaL_optnumber(state, 3, 0.0));
  const auto py = static_cast<float>(luaL_optnumber(state, 4, 0.0));
  const auto pz = static_cast<float>(luaL_optnumber(state, 5, 0.0));
  const std::uint32_t id = runtime_binding().services->add_ball_socket_joint(
      runtime_binding().world, entityA.index, entityB.index, px, py, pz);
  return push_joint_result(state, id);
}

// engine.add_slider_joint(entityA, entityB, axisX, axisY, axisZ)
// → joint_id | nil
int lua_engine_add_slider_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_slider_joint == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  const auto ax = static_cast<float>(luaL_optnumber(state, 3, 1.0));
  const auto ay = static_cast<float>(luaL_optnumber(state, 4, 0.0));
  const auto az = static_cast<float>(luaL_optnumber(state, 5, 0.0));
  const std::uint32_t id = runtime_binding().services->add_slider_joint(
      runtime_binding().world, entityA.index, entityB.index, ax, ay, az);
  return push_joint_result(state, id);
}

// engine.add_spring_joint(entityA, entityB, restLength, stiffness, damping)
// → joint_id | nil
int lua_engine_add_spring_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_spring_joint == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  const auto rest = static_cast<float>(luaL_optnumber(state, 3, 1.0));
  const auto stiff = static_cast<float>(luaL_optnumber(state, 4, 100.0));
  const auto damp = static_cast<float>(luaL_optnumber(state, 5, 1.0));
  const std::uint32_t id = runtime_binding().services->add_spring_joint(
      runtime_binding().world, entityA.index, entityB.index, rest, stiff, damp);
  return push_joint_result(state, id);
}

// engine.add_fixed_joint(entityA, entityB) → joint_id | nil
int lua_engine_add_fixed_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_fixed_joint == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  const std::uint32_t id =
      runtime_binding().services->add_fixed_joint(runtime_binding().world, entityA.index, entityB.index);
  return push_joint_result(state, id);
}

// engine.set_joint_limits(jointId, minLimit, maxLimit)
int lua_engine_set_joint_limits(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_joint_limits != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    const auto minL = static_cast<float>(luaL_optnumber(state, 2, 0.0));
    const auto maxL = static_cast<float>(luaL_optnumber(state, 3, 0.0));
    runtime_binding().services->set_joint_limits(runtime_binding().world, id, minL, maxL);
  }
  return 0;
}

// #125: get_half_extents/get_restitution/get_friction read through any
// same-frame queued collider write instead of only the committed snapshot.
int lua_engine_get_half_extents(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.x));
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.y));
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.z));
  return 3;
}

int lua_engine_set_half_extents(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 halfExtents{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &halfExtents)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.halfExtents = halfExtents;
  lua_pushboolean(state, apply_or_queue_collider(entity, collider) ? 1 : 0);
  return 1;
}

int lua_engine_get_restitution(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.restitution));
  return 1;
}

int lua_engine_get_friction(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!latest_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.staticFriction));
  lua_pushnumber(state, static_cast<lua_Number>(collider.dynamicFriction));
  return 2;
}

// --- MeshComponent: material getters/setters ---

} // namespace

void clear_lock_rotation_captures() noexcept {
  for (LockRotationCapture &capture : g_lockRotationCaptures) {
    capture = LockRotationCapture{};
  }
}

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_physics_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_add_collider);
  lua_setfield(state, -2, "add_collider");
  lua_pushcfunction(state, &lua_engine_add_capsule_collider);
  lua_setfield(state, -2, "add_capsule_collider");
  lua_pushcfunction(state, &lua_engine_set_restitution);
  lua_setfield(state, -2, "set_restitution");
  lua_pushcfunction(state, &lua_engine_set_lock_rotation);
  lua_setfield(state, -2, "set_lock_rotation");
  lua_pushcfunction(state, &lua_engine_set_friction);
  lua_setfield(state, -2, "set_friction");
  lua_pushcfunction(state, &lua_engine_create_physics_material);
  lua_setfield(state, -2, "create_physics_material");
  lua_pushcfunction(state, &lua_engine_set_collider_material);
  lua_setfield(state, -2, "set_collider_material");
  lua_pushcfunction(state, &lua_engine_set_collision_layer);
  lua_setfield(state, -2, "set_collision_layer");
  lua_pushcfunction(state, &lua_engine_set_collision_mask);
  lua_setfield(state, -2, "set_collision_mask");
  lua_pushcfunction(state, &lua_engine_set_gravity);
  lua_setfield(state, -2, "set_gravity");
  lua_pushcfunction(state, &lua_engine_get_gravity);
  lua_setfield(state, -2, "get_gravity");
  lua_pushcfunction(state, &lua_engine_raycast);
  lua_setfield(state, -2, "raycast");
  lua_pushcfunction(state, &lua_engine_raycast_all);
  lua_setfield(state, -2, "raycast_all");
  lua_pushcfunction(state, &lua_engine_overlap_sphere);
  lua_setfield(state, -2, "overlap_sphere");
  lua_pushcfunction(state, &lua_engine_overlap_box);
  lua_setfield(state, -2, "overlap_box");
  lua_pushcfunction(state, &lua_engine_sweep_sphere);
  lua_setfield(state, -2, "sweep_sphere");
  lua_pushcfunction(state, &lua_engine_sweep_box);
  lua_setfield(state, -2, "sweep_box");
  lua_pushcfunction(state, &lua_engine_add_distance_joint);
  lua_setfield(state, -2, "add_distance_joint");
  lua_pushcfunction(state, &lua_engine_add_hinge_joint);
  lua_setfield(state, -2, "add_hinge_joint");
  lua_pushcfunction(state, &lua_engine_add_ball_socket_joint);
  lua_setfield(state, -2, "add_ball_socket_joint");
  lua_pushcfunction(state, &lua_engine_add_slider_joint);
  lua_setfield(state, -2, "add_slider_joint");
  lua_pushcfunction(state, &lua_engine_add_spring_joint);
  lua_setfield(state, -2, "add_spring_joint");
  lua_pushcfunction(state, &lua_engine_add_fixed_joint);
  lua_setfield(state, -2, "add_fixed_joint");
  lua_pushcfunction(state, &lua_engine_set_joint_limits);
  lua_setfield(state, -2, "set_joint_limits");
  lua_pushcfunction(state, &lua_engine_remove_joint);
  lua_setfield(state, -2, "remove_joint");
  lua_pushcfunction(state, &lua_engine_get_half_extents);
  lua_setfield(state, -2, "get_half_extents");
  lua_pushcfunction(state, &lua_engine_set_half_extents);
  lua_setfield(state, -2, "set_half_extents");
  lua_pushcfunction(state, &lua_engine_get_restitution);
  lua_setfield(state, -2, "get_restitution");
  lua_pushcfunction(state, &lua_engine_get_friction);
  lua_setfield(state, -2, "get_friction");
}

} // namespace engine::scripting
