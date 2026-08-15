// Implements the built-in bootstrap content: registration of the procedural
// primitive meshes and creation of the default editor scene (ground, demo
// cubes, sun light, foliage patch, scene controller).

#include "engine_bootstrap_content.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/renderer/material_loader.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/runtime/world.h"

namespace engine {

bool resolve_mesh_asset_path(char *outPath, std::size_t outCapacity) noexcept {
  if ((outPath == nullptr) || (outCapacity == 0U)) {
    return false;
  }

  const char *virtualPath = active_config().bootstrapMeshPath;
  if ((virtualPath == nullptr) || (virtualPath[0] == '\0')) {
    return false;
  }
  return core::vfs_resolve_os_path(virtualPath, outPath, outCapacity);
}

renderer::AssetId register_builtin_mesh(renderer::GpuMeshRegistry *registry,
                                        renderer::AssetDatabase *database,
                                        const renderer::GpuMesh &mesh,
                                        const char *builtinPath) noexcept {
  const renderer::MeshHandle handle = renderer::register_gpu_mesh(registry, mesh);
  if (handle == renderer::kInvalidMeshHandle) {
    return renderer::kInvalidAssetId;
  }
  const renderer::AssetId id = renderer::make_asset_id_from_path(builtinPath);
  if (id == renderer::kInvalidAssetId) {
    return renderer::kInvalidAssetId;
  }
  if (!renderer::register_mesh_asset(database, id, builtinPath, handle)) {
    return renderer::kInvalidAssetId;
  }
  const std::uint64_t vertexFloats = mesh.hasUVs ? 8ULL : 6ULL;
  const std::uint64_t sizeEstimate =
      (static_cast<std::uint64_t>(mesh.vertexCount) * vertexFloats *
       sizeof(float)) +
      (static_cast<std::uint64_t>(mesh.indexCount) * sizeof(std::uint32_t));
  static_cast<void>(renderer::set_mesh_asset_size(database, id, sizeEstimate));
  return id;
}

// ---------------------------------------------------------------------------
// Job functions
// ---------------------------------------------------------------------------


/// Loads the requested resource for bootstrap meshes.
bool load_bootstrap_meshes(renderer::AssetManager *assetManager,
                           renderer::AssetDatabase *assetDatabase,
                           renderer::GpuMeshRegistry *meshRegistry,
                           BootstrapMeshIds *out) noexcept {
  char meshPath[512]{};
  if (!resolve_mesh_asset_path(meshPath, sizeof(meshPath))) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to resolve mesh asset path");
    return false;
  }

  out->bootstrap = renderer::make_asset_id_from_file(meshPath);
  bool ok = (out->bootstrap != renderer::kInvalidAssetId) &&
            renderer::queue_mesh_load(assetManager, assetDatabase,
                                      out->bootstrap, meshPath);
  if (ok) {
    if (!core::make_render_context_current()) {
      core::log_message(
          core::LogLevel::Error, "engine",
          "failed to acquire OpenGL context for bootstrap mesh upload");
      return false;
    }
    ok = renderer::update_asset_manager(assetManager, assetDatabase,
                                        meshRegistry, 8U);
    core::release_render_context();
    ok = ok && (renderer::mesh_asset_state(assetDatabase, out->bootstrap) ==
                renderer::AssetState::Ready);
  }
  if (!ok) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to load bootstrap mesh asset");
    return false;
  }

  if (!core::make_render_context_current()) {
    core::log_message(
        core::LogLevel::Warning, "engine",
        "failed to acquire OpenGL context for procedural mesh upload");
  } else {
    renderer::GpuMesh m{};
    if (renderer::build_plane_mesh(&m)) {
      out->plane = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                         "builtin://plane");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_cube_mesh(&m)) {
      out->cube = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                        "builtin://cube");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_sphere_mesh(&m)) {
      out->sphere = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                          "builtin://sphere");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_cylinder_mesh(&m)) {
      out->cylinder = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                            "builtin://cylinder");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_capsule_mesh(&m)) {
      out->capsule = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                           "builtin://capsule");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_pyramid_mesh(&m)) {
      out->pyramid = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                           "builtin://pyramid");
    }
    m = renderer::GpuMesh{};
    if (renderer::build_grass_tuft_mesh(&m)) {
      out->grass = register_builtin_mesh(meshRegistry, assetDatabase, m,
                                         "builtin://grass");
    }

    // Bundled rigged character (cooked skinned .mesh loaded from disk).
    {
      char characterVirtualPath[512] = {};
      std::snprintf(characterVirtualPath, sizeof(characterVirtualPath),
                    "%s/character.mesh", active_config().assetMount);
      char characterPath[512] = {};
      if (core::vfs_resolve_os_path(characterVirtualPath, characterPath,
                                    sizeof(characterPath))) {
        const renderer::AssetId characterId =
            renderer::make_asset_id_from_path(characterVirtualPath);
        if (renderer::queue_mesh_load(assetManager, assetDatabase,
                                      characterId, characterPath) &&
            renderer::update_asset_manager(assetManager, assetDatabase,
                                           meshRegistry, 8U) &&
            (renderer::mesh_asset_state(assetDatabase, characterId) ==
             renderer::AssetState::Ready)) {
          out->character = characterId;
        } else {
          core::log_message(core::LogLevel::Warning, "engine",
                            "rigged character mesh failed to load");
        }
      }
    }
    core::release_render_context();
  }

  // Discover project material JSONs so MeshComponent.materialAssetId
  // references resolve during render prep.
  char materialsDir[512] = {};
  std::snprintf(materialsDir, sizeof(materialsDir), "%s/materials",
                active_config().assetRoot);
  char materialsPrefix[512] = {};
  std::snprintf(materialsPrefix, sizeof(materialsPrefix), "%s/materials",
                active_config().assetMount);
  const std::size_t materialCount = renderer::load_material_assets_in_directory(
      assetDatabase, materialsDir, materialsPrefix);
  if (materialCount > 0U) {
    char logBuffer[128] = {};
    std::snprintf(logBuffer, sizeof(logBuffer), "loaded %zu material assets",
                  materialCount);
    core::log_message(core::LogLevel::Info, "assets", logBuffer);
  }

  return true;
}

// ---------------------------------------------------------------------------
// Bootstrap scene
// ---------------------------------------------------------------------------

void create_bootstrap_scene(runtime::World *world,
                            const BootstrapMeshIds &meshIds) noexcept {
  const renderer::AssetId defaultMesh =
      (meshIds.cube != renderer::kInvalidAssetId) ? meshIds.cube
                                                  : meshIds.bootstrap;

  const runtime::Entity entity = world->create_scene_object();
  const runtime::Entity stackedEntity = world->create_scene_object();
  const runtime::Entity groundEntity = world->create_scene_object();
  const runtime::Entity foliageEntity = world->create_scene_object();
  const runtime::Entity lightEntity = world->create_scene_object();
  const runtime::Entity sceneControllerEntity = world->create_scene_object();
  const runtime::Entity characterEntity =
      (meshIds.character != renderer::kInvalidAssetId)
          ? world->create_scene_object()
          : runtime::kInvalidEntity;
  if ((entity == runtime::kInvalidEntity) ||
      (stackedEntity == runtime::kInvalidEntity) ||
      (groundEntity == runtime::kInvalidEntity) ||
      (foliageEntity == runtime::kInvalidEntity) ||
      (lightEntity == runtime::kInvalidEntity) ||
      (sceneControllerEntity == runtime::kInvalidEntity)) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to create bootstrap entities");
    return;
  }

  auto add_name = [&](runtime::Entity e, const char *label) {
    runtime::NameComponent n{};
    std::snprintf(n.name, sizeof(n.name), "%s", label);
    static_cast<void>(world->add_name_component(e, n));
  };
  add_name(entity, "Red Cube");
  add_name(stackedEntity, "Blue Cube");
  add_name(groundEntity, "Ground");
  add_name(foliageEntity, "Foliage Patch");
  add_name(lightEntity, "Sun Light");
  add_name(sceneControllerEntity, "Scene Controller");
  if (characterEntity != runtime::kInvalidEntity) {
    add_name(characterEntity, "Character");
  }

  // Rigged character: skinned mesh plus the idle/walk/jump controller.
  if (characterEntity != runtime::kInvalidEntity) {
    runtime::Transform t{};
    t.position = math::Vec3(-3.5F, 0.0F, 1.5F);
    static_cast<void>(world->add_transform(characterEntity, t));
    runtime::MeshComponent mc{};
    mc.meshAssetId = meshIds.character;
    mc.albedo = math::Vec3(0.85F, 0.65F, 0.35F);
    static_cast<void>(world->add_mesh_component(characterEntity, mc));
    runtime::AnimationComponent anim{};
    std::snprintf(anim.controllerPath, sizeof(anim.controllerPath),
                  "%s/character.animctrl.json", active_config().assetMount);
    static_cast<void>(world->add_animation_component(characterEntity, anim));
  }

  // Directional light.
  {
    runtime::Transform lt{};
    lt.position = math::Vec3(0.0F, 10.0F, 0.0F);
    static_cast<void>(world->add_transform(lightEntity, lt));
    runtime::LightComponent sunLight{};
    sunLight.type = runtime::LightType::Directional;
    sunLight.color = math::Vec3(1.0F, 0.95F, 0.9F);
    sunLight.direction = math::Vec3(0.4F, -1.0F, 0.6F);
    sunLight.intensity = 1.2F;
    static_cast<void>(world->add_light_component(lightEntity, sunLight));
  }

  // Red cube.
  {
    runtime::Transform t{};
    t.position = math::Vec3(-3.0F, 0.5F, -3.0F);
    static_cast<void>(world->add_transform(entity, t));
    runtime::RigidBody rb{};
    rb.velocity = math::Vec3(0.0F, 0.0F, 0.0F);
    rb.inverseMass = 0.0F;
    static_cast<void>(world->add_rigid_body(entity, rb));
    runtime::Collider c{};
    c.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    static_cast<void>(world->add_collider(entity, c));
    runtime::MeshComponent mc{};
    mc.meshAssetId = defaultMesh;
    mc.albedo = math::Vec3(0.9F, 0.2F, 0.2F);
    static_cast<void>(world->add_mesh_component(entity, mc));
  }

  // Blue cube.
  {
    runtime::Transform t{};
    t.position = math::Vec3(3.0F, 0.5F, -3.0F);
    static_cast<void>(world->add_transform(stackedEntity, t));
    runtime::RigidBody rb{};
    rb.velocity = math::Vec3(0.0F, 0.0F, 0.0F);
    rb.inverseMass = 0.0F;
    static_cast<void>(world->add_rigid_body(stackedEntity, rb));
    runtime::Collider c{};
    c.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    static_cast<void>(world->add_collider(stackedEntity, c));
    runtime::MeshComponent mc{};
    mc.meshAssetId = defaultMesh;
    mc.albedo = math::Vec3(0.2F, 0.4F, 0.9F);
    static_cast<void>(world->add_mesh_component(stackedEntity, mc));
  }

  // Ground plane.
  {
    runtime::Transform t{};
    t.position = math::Vec3(0.0F, -0.5F, 0.0F);
    static_cast<void>(world->add_transform(groundEntity, t));
    runtime::Collider gc{};
    gc.halfExtents = math::Vec3(5.0F, 0.5F, 5.0F);
    gc.staticFriction = 0.9F;
    gc.dynamicFriction = 0.7F;
    gc.restitution = 0.1F;
    static_cast<void>(world->add_collider(groundEntity, gc));
    runtime::MeshComponent mc{};
    mc.meshAssetId = (meshIds.plane != renderer::kInvalidAssetId)
                         ? meshIds.plane
                         : meshIds.bootstrap;
    mc.albedo = math::Vec3(0.45F, 0.42F, 0.38F);
    static_cast<void>(world->add_mesh_component(groundEntity, mc));
  }

  // Foliage patch demo.
  {
    runtime::Transform t{};
    t.position = math::Vec3(0.0F, 0.0F, 1.3F);
    static_cast<void>(world->add_transform(foliageEntity, t));

    runtime::FoliagePatchComponent foliage{};
    foliage.meshAssetIds[0] = (meshIds.grass != renderer::kInvalidAssetId)
                                  ? meshIds.grass
                                  : defaultMesh;
    foliage.meshAssetIds[1] = foliage.meshAssetIds[0];
    foliage.meshAssetIds[2] = foliage.meshAssetIds[1];
    foliage.instanceCount = 35U;
    foliage.density = 2.5F;
    foliage.albedo = math::Vec3(0.18F, 0.62F, 0.22F);
    foliage.roughness = 0.92F;
    foliage.windStrength = 0.18F;
    foliage.windFrequency = 1.9F;

    std::uint32_t cursor = 0U;
    for (std::uint32_t z = 0U; z < 5U; ++z) {
      for (std::uint32_t x = 0U; x < 7U; ++x) {
        runtime::FoliageInstance &instance = foliage.instances[cursor];
        const float jitterX =
            (static_cast<float>((x * 17U + z * 11U) % 5U) - 2.0F) * 0.05F;
        const float jitterZ =
            (static_cast<float>((x * 7U + z * 19U) % 5U) - 2.0F) * 0.05F;
        instance.scale = 0.55F + (static_cast<float>((x + z) % 4U) * 0.08F);
        instance.offset =
            math::Vec3((static_cast<float>(x) - 3.0F) * 0.62F + jitterX, 0.0F,
                       (static_cast<float>(z) - 2.0F) * 0.62F + jitterZ);
        instance.phase = static_cast<float>(cursor) * 0.37F;
        instance.lodIndex = ((x + z) % 5U == 0U) ? 1U : 0U;
        ++cursor;
      }
    }

    static_cast<void>(
        world->add_foliage_patch_component(foliageEntity, foliage));
  }

  // Scene controller script.
  {
    runtime::ScriptComponent sc{};
    const char *mainScriptPath = active_config().mainScriptPath;
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s",
                  (mainScriptPath != nullptr) ? mainScriptPath : "");
    static_cast<void>(world->add_script_component(sceneControllerEntity, sc));
  }
}

// ---------------------------------------------------------------------------
// Scene light collection
// ---------------------------------------------------------------------------


} // namespace engine
