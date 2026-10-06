// Implements mesh and material Lua bindings (mesh assignment, procedural
// shape spawning, PBR material fields)
// for the scripting module. Split out of scripting.cpp (REVIEW_FINDINGS A3).

#include "mesh_material_bindings.h"

#include "binding_util.h"
#include "deferred_mutations.h"
#include "entity_handle.h"
#include "lua_math_values.h"
#include "lua_state.h"
#include "reload_transaction.h"
#include "runtime_binding.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdint>
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

std::uint64_t g_defaultMeshAssetId = 0ULL;
std::uint64_t g_builtinPlaneMesh = 0ULL;
std::uint64_t g_builtinCubeMesh = 0ULL;
std::uint64_t g_builtinSphereMesh = 0ULL;
std::uint64_t g_builtinCylinderMesh = 0ULL;
std::uint64_t g_builtinCapsuleMesh = 0ULL;
std::uint64_t g_builtinPyramidMesh = 0ULL;

/// The identity the catalog holds for an asset id, so a component a
/// script points at an asset carries what a saved scene names and not
/// only the id the bytes sit under this session. Nil when the runtime
/// publishes no lookup or the asset carries no identity.
core::AssetRef catalogued_asset_ref(std::uint64_t assetId) noexcept {
  const RuntimeServices *services = runtime_binding().services;
  if ((services == nullptr) || (services->asset_ref_for_id == nullptr)) {
    return core::AssetRef{};
  }
  return services->asset_ref_for_id(assetId);
}

int lua_engine_set_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const auto meshId = static_cast<std::uint64_t>(lua_tointeger(state, 2));
  if (meshId == 0ULL) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  static_cast<void>(latest_mesh_component(entity, &component));

  component.meshAssetId = meshId;
  component.meshRef = catalogued_asset_ref(meshId);
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

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
// The collider model mirrors Unity/Unreal: box, sphere, and capsule are
// the analytic primitives (sphere encodes radius in halfExtents.x;
// capsule encodes radius in .x and half height in .y); cylinder and
// pyramid collide as convex hulls that match their render meshes, and
// degrade to the bounding box if hull slots are exhausted so the prop
// still collides instead of falling through the world. A plane's thin box
// sits below its surface, so what stands on it rests on the ground drawn.
// The collider is runtime::primitive_collider's, the same one the
// editor's Create menu installs.
int lua_engine_spawn_shape(lua_State *state) noexcept {
  if (!can_create_entities_now() || !lua_isstring(state, 1) ||
      (reload_staging(ReloadEffect::CreateEntity) == ReloadStaging::Refused)) {
    lua_pushnil(state);
    return 1;
  }

  math::Vec3 pos{};
  math::Vec3 albedo(1.0F, 1.0F, 1.0F);
  int vectorArg = 2;
  if (!read_vec3_arg(state, &vectorArg, &pos)) {
    lua_pushnil(state);
    return 1;
  }
  // The colour is optional and passed on as given, a non-finite one
  // included, so the World refuses it and the spawn fails whole.
  if (!to_vec3_value(state, vectorArg, &albedo) &&
      lua_isnumber(state, vectorArg) && lua_isnumber(state, vectorArg + 1) &&
      lua_isnumber(state, vectorArg + 2)) {
    albedo.x = static_cast<float>(lua_tonumber(state, vectorArg));
    albedo.y = static_cast<float>(lua_tonumber(state, vectorArg + 1));
    albedo.z = static_cast<float>(lua_tonumber(state, vectorArg + 2));
  }

  const char *shape = lua_tostring(state, 1);

  // The name picks the primitive and its mesh; the collider is the one
  // description the runtime tier gives every spawn path (the editor's
  // Create menu included), hull provenance and a plane's offset below its
  // surface with it, so a script spawn never describes one of its own.
  struct ShapeRow final {
    const char *name;
    math::PrimitiveShape shape;
    std::uint64_t mesh;
  };
  const ShapeRow rows[] = {
      {"cube", math::PrimitiveShape::Cube, g_builtinCubeMesh},
      {"sphere", math::PrimitiveShape::Sphere, g_builtinSphereMesh},
      {"cylinder", math::PrimitiveShape::Cylinder, g_builtinCylinderMesh},
      {"capsule", math::PrimitiveShape::Capsule, g_builtinCapsuleMesh},
      {"pyramid", math::PrimitiveShape::Pyramid, g_builtinPyramidMesh},
      {"plane", math::PrimitiveShape::Plane, g_builtinPlaneMesh},
  };
  const ShapeRow *row = nullptr;
  for (const ShapeRow &candidate : rows) {
    if (std::strcmp(shape, candidate.name) == 0) {
      row = &candidate;
    }
  }
  if (row == nullptr) {
    // An unrecognized shape name previously fell through to the default cube
    // mesh with a box collider, so a typo silently produced the wrong object.
    core::log_message(core::LogLevel::Warning, "scripting",
                      "spawn_shape rejected an unknown shape name");
    lua_pushnil(state);
    return 1;
  }
  const std::uint64_t meshId =
      (row->mesh != 0ULL) ? row->mesh : g_defaultMeshAssetId;

  // World::add_collider rebuilds a hull's payload from the provenance tag,
  // so a script spawn never builds or carries a physics hull of its own.
  const RuntimeServices &services = *runtime_binding().services;
  runtime::World *const world = runtime_binding().world;
  runtime::Collider collider = services.primitive_collider(row->shape);
  const bool hasHull = (collider.shape == runtime::ColliderShape::ConvexHull);

  runtime::Transform transform{};
  transform.position = pos;
  const runtime::Entity entity =
      services.create_scene_object_op(world, &transform);
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  // Every insertion below is checked so a partially constructed entity is
  // rolled back instead of leaking to Lua as a success.
  const char *failedStep = nullptr;

  runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 1.0F;
  if (!services.add_rigid_body_op(world, entity, rigidBody)) {
    failedStep = "rigid body";
  }

  if (failedStep == nullptr) {
    if (!services.add_collider_op(world, entity, collider)) {
      failedStep = "collider";
    } else if (hasHull && !services.has_convex_hull_payload(world, entity)) {
      // Documented fallback: hull slots exhausted degrades to the bounding
      // box, but only if the replacement collider actually installs.
      core::log_message(
          core::LogLevel::Warning, "scripting",
          "spawn_shape hull slots exhausted — using box collider");
      collider.shape = runtime::ColliderShape::AABB;
      collider.hullSource = runtime::HullSource::None;
      if (!services.add_collider_op(world, entity, collider)) {
        failedStep = "fallback collider";
      }
    }
  }

  if ((failedStep == nullptr) && (meshId != 0ULL)) {
    runtime::MeshComponent meshComp{};
    meshComp.meshAssetId = meshId;
    meshComp.meshRef = catalogued_asset_ref(meshId);
    meshComp.albedo = albedo;
    if (!services.add_mesh_component_op(world, entity, meshComp)) {
      failedStep = "mesh component";
    }
  }

  if (failedStep != nullptr) {
    char message[128] = {};
    std::snprintf(message, sizeof(message),
                  "spawn_shape %s insertion failed — spawn rolled back",
                  failedStep);
    core::log_message(core::LogLevel::Warning, "scripting", message);
    static_cast<void>(services.destroy_entity_op(world, entity));
    lua_pushnil(state);
    return 1;
  }

  reload_note_created_entity(entity);
  push_entity_handle(state, entity);
  return 1;
}

int lua_engine_set_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 albedo{};
  int vectorArg = 2;
  if (!read_entity(state, 1, &entity) || !read_vec3_arg(state, &vectorArg, &albedo)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  static_cast<void>(latest_mesh_component(entity, &component));

  component.albedo = albedo;
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// get_albedo/get_mesh/get_roughness/get_metallic/get_opacity read
// through any same-frame queued mesh-component write instead of only the
// committed snapshot (copied out, mirroring the latest_mesh_component
// helper's value semantics rather than the old pointer-into-World read).
int lua_engine_get_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh.albedo.x));
  lua_pushnumber(state, static_cast<lua_Number>(mesh.albedo.y));
  lua_pushnumber(state, static_cast<lua_Number>(mesh.albedo.z));
  return 3;
}

int lua_engine_get_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(mesh.meshAssetId));
  return 1;
}

int lua_engine_set_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  float value = 0.0F;
  if (!read_entity(state, 1, &entity) ||
      !read_finite_number_arg(state, 2, &value)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.roughness = value;
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

int lua_engine_get_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh.roughness));
  return 1;
}

int lua_engine_set_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  float value = 0.0F;
  if (!read_entity(state, 1, &entity) ||
      !read_finite_number_arg(state, 2, &value)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.metallic = value;
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

int lua_engine_get_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh.metallic));
  return 1;
}

int lua_engine_set_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  float value = 0.0F;
  if (!read_entity(state, 1, &entity) ||
      !read_finite_number_arg(state, 2, &value)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.opacity = value;
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

int lua_engine_get_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!latest_mesh_component(entity, &mesh)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh.opacity));
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
