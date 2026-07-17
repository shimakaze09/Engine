// Implements mesh and material Lua bindings (mesh assignment, procedural
// shape spawning, PBR material fields)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "mesh_material_bindings.h"

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

std::uint64_t g_defaultMeshAssetId = 0ULL;
std::uint64_t g_builtinPlaneMesh = 0ULL;
std::uint64_t g_builtinCubeMesh = 0ULL;
std::uint64_t g_builtinSphereMesh = 0ULL;
std::uint64_t g_builtinCylinderMesh = 0ULL;
std::uint64_t g_builtinCapsuleMesh = 0ULL;
std::uint64_t g_builtinPyramidMesh = 0ULL;

/// Handles lua engine set mesh.
int lua_engine_set_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer meshId = lua_tointeger(state, 2);
  if (meshId <= 0) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  const runtime::MeshComponent *existing =
      runtime_binding().world->get_mesh_component_ptr(entity);
  if (existing != nullptr) {
    component = *existing;
  }

  component.meshAssetId = static_cast<std::uint64_t>(meshId);
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get default mesh asset id.
int lua_engine_get_default_mesh_asset_id(lua_State *state) noexcept {
  if (g_defaultMeshAssetId == 0ULL) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(g_defaultMeshAssetId));
  return 1;
}

// engine.spawn_shape(shape, x, y, z, r, g, b) → entity index or nil
// shape: "cube" | "sphere" | "cylinder" | "capsule" | "pyramid" | "plane"
// Spawns a physics entity with the matching mesh and appropriate collider.
int lua_engine_spawn_shape(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !can_apply_mutations_now() ||
      !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  math::Vec3 pos{};
  math::Vec3 albedo(1.0F, 1.0F, 1.0F);
  if (!read_vec3_args(state, 2, &pos)) {
    lua_pushnil(state);
    return 1;
  }
  if (lua_isnumber(state, 5) && lua_isnumber(state, 6) &&
      lua_isnumber(state, 7)) {
    albedo.x = static_cast<float>(lua_tonumber(state, 5));
    albedo.y = static_cast<float>(lua_tonumber(state, 6));
    albedo.z = static_cast<float>(lua_tonumber(state, 7));
  }

  const char *shape = lua_tostring(state, 1);

  std::uint64_t meshId = g_defaultMeshAssetId;
  math::Vec3 halfExtents(0.5F, 0.5F, 0.5F);
  runtime::ColliderShape colliderShape = runtime::ColliderShape::AABB;

  if (std::strcmp(shape, "cube") == 0) {
    meshId =
        (g_builtinCubeMesh != 0ULL) ? g_builtinCubeMesh : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "sphere") == 0) {
    meshId = (g_builtinSphereMesh != 0ULL) ? g_builtinSphereMesh
                                           : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F); // halfExtents.x = radius
    colliderShape = runtime::ColliderShape::Sphere;
  } else if (std::strcmp(shape, "cylinder") == 0) {
    meshId = (g_builtinCylinderMesh != 0ULL) ? g_builtinCylinderMesh
                                             : g_defaultMeshAssetId;
    // Best available approximation for a round cylinder: upright capsule.
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::Capsule;
  } else if (std::strcmp(shape, "capsule") == 0) {
    meshId = (g_builtinCapsuleMesh != 0ULL) ? g_builtinCapsuleMesh
                                            : g_defaultMeshAssetId;
    // Capsule: halfExtents.x = radius, halfExtents.y = halfHeight.
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::Capsule;
  } else if (std::strcmp(shape, "pyramid") == 0) {
    meshId = (g_builtinPyramidMesh != 0ULL) ? g_builtinPyramidMesh
                                            : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.58F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "plane") == 0) {
    meshId = (g_builtinPlaneMesh != 0ULL) ? g_builtinPlaneMesh
                                          : g_defaultMeshAssetId;
    halfExtents = math::Vec3(5.0F, 0.1F, 5.0F);
    colliderShape = runtime::ColliderShape::AABB;
  }

  const runtime::Entity entity = runtime_binding().world->create_entity();
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  runtime::Transform transform{};
  transform.position = pos;
  static_cast<void>(runtime_binding().world->add_transform(entity, transform));

  runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 1.0F;
  static_cast<void>(runtime_binding().world->add_rigid_body(entity, rigidBody));

  runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  collider.shape = colliderShape;
  static_cast<void>(runtime_binding().world->add_collider(entity, collider));

  if (meshId != 0ULL) {
    runtime::MeshComponent meshComp{};
    meshComp.meshAssetId = meshId;
    meshComp.albedo = albedo;
    static_cast<void>(runtime_binding().world->add_mesh_component(entity, meshComp));
  }

  push_entity_handle(state, entity);
  return 1;
}

/// Handles lua engine set albedo.
int lua_engine_set_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 albedo{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &albedo)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  const runtime::MeshComponent *existing =
      runtime_binding().world->get_mesh_component_ptr(entity);
  if (existing != nullptr) {
    component = *existing;
  }

  component.albedo = albedo;
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.x));
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.y));
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.z));
  return 3;
}

/// Handles lua engine get mesh.
int lua_engine_get_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(mesh->meshAssetId));
  return 1;
}

/// Handles lua engine set roughness.
int lua_engine_set_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!runtime_binding().world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.roughness = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

/// Handles lua engine get roughness.
int lua_engine_get_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->roughness));
  return 1;
}

/// Handles lua engine set metallic.
int lua_engine_set_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!runtime_binding().world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.metallic = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

/// Handles lua engine get metallic.
int lua_engine_get_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->metallic));
  return 1;
}

/// Handles lua engine set opacity.
int lua_engine_set_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!runtime_binding().world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.opacity = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

/// Handles lua engine get opacity.
int lua_engine_get_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->opacity));
  return 1;
}

// --- LightComponent ---

} // namespace

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_mesh_material_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_set_mesh);
  lua_setfield(state, -2, "set_mesh");
  lua_pushcfunction(state, &lua_engine_get_default_mesh_asset_id);
  lua_setfield(state, -2, "get_default_mesh_asset_id");
  lua_pushcfunction(state, &lua_engine_spawn_shape);
  lua_setfield(state, -2, "spawn_shape");
  lua_pushcfunction(state, &lua_engine_set_albedo);
  lua_setfield(state, -2, "set_albedo");
  lua_pushcfunction(state, &lua_engine_get_albedo);
  lua_setfield(state, -2, "get_albedo");
  lua_pushcfunction(state, &lua_engine_get_mesh);
  lua_setfield(state, -2, "get_mesh");
  lua_pushcfunction(state, &lua_engine_set_roughness);
  lua_setfield(state, -2, "set_roughness");
  lua_pushcfunction(state, &lua_engine_get_roughness);
  lua_setfield(state, -2, "get_roughness");
  lua_pushcfunction(state, &lua_engine_set_metallic);
  lua_setfield(state, -2, "set_metallic");
  lua_pushcfunction(state, &lua_engine_get_metallic);
  lua_setfield(state, -2, "get_metallic");
  lua_pushcfunction(state, &lua_engine_set_opacity);
  lua_setfield(state, -2, "set_opacity");
  lua_pushcfunction(state, &lua_engine_get_opacity);
  lua_setfield(state, -2, "get_opacity");
}

/// Sets the requested value for default mesh asset id.
void set_default_mesh_asset_id(std::uint64_t assetId) noexcept {
  g_defaultMeshAssetId = assetId;
}

/// Sets the requested value for builtin mesh ids.
void set_builtin_mesh_ids(std::uint64_t planeMesh, std::uint64_t cubeMesh,
                          std::uint64_t sphereMesh, std::uint64_t cylinderMesh,
                          std::uint64_t capsuleMesh,
                          std::uint64_t pyramidMesh) noexcept {
  g_builtinPlaneMesh = planeMesh;
  g_builtinCubeMesh = cubeMesh;
  g_builtinSphereMesh = sphereMesh;
  g_builtinCylinderMesh = cylinderMesh;
  g_builtinCapsuleMesh = capsuleMesh;
  g_builtinPyramidMesh = pyramidMesh;
}

/// Clears the default/builtin mesh asset ids (engine shutdown).
void reset_mesh_material_bindings() noexcept {
  g_defaultMeshAssetId = 0ULL;
  g_builtinPlaneMesh = 0ULL;
  g_builtinCubeMesh = 0ULL;
  g_builtinSphereMesh = 0ULL;
  g_builtinCylinderMesh = 0ULL;
  g_builtinCapsuleMesh = 0ULL;
  g_builtinPyramidMesh = 0ULL;
}

} // namespace engine::scripting
