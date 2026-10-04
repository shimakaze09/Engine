// Implements the built-in bootstrap content: registration of the procedural
// primitive meshes and creation of the startup scene, the empty 3D template
// Unity starts a project from (a main camera and a directional light) plus
// the scene controller that runs the project's main Lua module.

#include "engine_bootstrap_content.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_references.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/renderer/asset_manager.h"
#include "engine/renderer/material_loader.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/mesh_primitives.h"
#include "engine/runtime/content_catalog.h"
#include "engine/runtime/world.h"

namespace engine {

namespace {

/// Loads the materials in `<root>/materials` as `<mount>/materials/...`.
std::size_t load_materials_of(renderer::AssetDatabase *assetDatabase,
                              content::AssetCatalog *catalog, const char *mount,
                              const char *root) noexcept {
  char materialsDir[512] = {};
  std::snprintf(materialsDir, sizeof(materialsDir), "%s/materials", root);
  char materialsPrefix[512] = {};
  std::snprintf(materialsPrefix, sizeof(materialsPrefix), "%s/materials",
                mount);
  return renderer::load_material_assets_in_directory(
      assetDatabase, catalog, materialsDir, materialsPrefix);
}

} // namespace

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

content::AssetId register_builtin_mesh(renderer::GpuMeshRegistry *registry,
                                       renderer::AssetDatabase *database,
                                       content::AssetCatalog *catalog,
                                       const renderer::GpuMesh &mesh,
                                       content::BuiltinMesh builtin) noexcept {
  const char *builtinPath = content::builtin_mesh_path(builtin);
  const renderer::MeshHandle handle = renderer::register_gpu_mesh(registry, mesh);
  if (handle == renderer::kInvalidMeshHandle) {
    return content::kInvalidAssetId;
  }
  const content::AssetId id = content::make_asset_id_from_path(builtinPath);
  if (id == content::kInvalidAssetId) {
    return content::kInvalidAssetId;
  }
  if (!renderer::register_mesh_asset(database, id, builtinPath, handle)) {
    return content::kInvalidAssetId;
  }
  // The catalog lists the primitive beside the project's meshes so a
  // picker offers it and a saved reference to it resolves; the record is
  // content's, which engine_validate catalogues the same way.
  static_cast<void>(content::register_builtin_mesh_record(catalog, builtin));
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
                           content::AssetCatalog *catalog,
                           renderer::GpuMeshRegistry *meshRegistry,
                           BootstrapMeshIds *out) noexcept {
  char meshPath[512]{};
  if (!resolve_mesh_asset_path(meshPath, sizeof(meshPath))) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to resolve mesh asset path");
    return false;
  }

  out->bootstrap = content::make_asset_id_from_file(meshPath);
  bool ok = (out->bootstrap != content::kInvalidAssetId) &&
            renderer::queue_mesh_load(assetManager, assetDatabase,
                                      out->bootstrap, meshPath);
  if (ok) {
    ok = renderer::update_asset_manager(assetManager, assetDatabase,
                                        meshRegistry, 8U);
    ok = ok && (renderer::mesh_asset_state(assetDatabase, out->bootstrap) ==
                content::AssetState::Ready);
  }
  if (!ok) {
    core::log_message(core::LogLevel::Error, "engine",
                      "failed to load bootstrap mesh asset");
    return false;
  }

  renderer::GpuMesh m{};
  if (renderer::build_plane_mesh(&m)) {
    out->plane = register_builtin_mesh(meshRegistry, assetDatabase, catalog, m,
                                       content::BuiltinMesh::Plane);
  }
  m = renderer::GpuMesh{};
  if (renderer::build_cube_mesh(&m)) {
    out->cube = register_builtin_mesh(meshRegistry, assetDatabase, catalog, m,
                                      content::BuiltinMesh::Cube);
  }
  m = renderer::GpuMesh{};
  if (renderer::build_sphere_mesh(&m)) {
    out->sphere = register_builtin_mesh(meshRegistry, assetDatabase, catalog, m,
                                        content::BuiltinMesh::Sphere);
  }
  m = renderer::GpuMesh{};
  if (renderer::build_cylinder_mesh(&m)) {
    out->cylinder = register_builtin_mesh(meshRegistry, assetDatabase, catalog,
                                          m, content::BuiltinMesh::Cylinder);
  }
  m = renderer::GpuMesh{};
  if (renderer::build_capsule_mesh(&m)) {
    out->capsule = register_builtin_mesh(meshRegistry, assetDatabase, catalog,
                                         m, content::BuiltinMesh::Capsule);
  }
  m = renderer::GpuMesh{};
  if (renderer::build_pyramid_mesh(&m)) {
    out->pyramid = register_builtin_mesh(meshRegistry, assetDatabase, catalog,
                                         m, content::BuiltinMesh::Pyramid);
  }
  m = renderer::GpuMesh{};
  if (renderer::build_grass_tuft_mesh(&m)) {
    out->grass = register_builtin_mesh(meshRegistry, assetDatabase, catalog, m,
                                       content::BuiltinMesh::GrassTuft);
  }

  // Catalogue the mounts so a saved mesh id maps back to the path its
  // bytes live at and a picker can list what exists before it loads. A
  // loader's own record wins: the walk never replaces one that exists,
  // and the material loader below updates the walk's in place. The walk
  // logs its own outcome, an Error naming every offending path when the
  // mount does not index cleanly, so nothing here repeats it. With no
  // project open the engine's own content is all there is.
  static_cast<void>(
      runtime::catalogue_engine_content(catalog, active_config()));
  if (!has_open_project()) {
    return true;
  }

  // Discover the material JSONs of the project and of each package so
  // MeshComponent.materialAssetId references resolve during render prep.
  std::size_t materialCount =
      load_materials_of(assetDatabase, catalog, active_config().assetMount,
                        active_config().assetRoot);
  for (std::size_t i = 0U; i < active_config().packageCount; ++i) {
    materialCount += load_materials_of(assetDatabase, catalog,
                                       active_config().packages[i].mount,
                                       active_config().packages[i].root);
  }
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

void create_bootstrap_scene(runtime::World *world) noexcept {
  const runtime::Entity cameraEntity = world->create_scene_object();
  const runtime::Entity lightEntity = world->create_scene_object();
  const runtime::Entity sceneControllerEntity = world->create_scene_object();
  if ((cameraEntity == runtime::kInvalidEntity) ||
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
  add_name(cameraEntity, "Main Camera");
  add_name(lightEntity, "Directional Light");
  add_name(sceneControllerEntity, "Scene Controller");

  // Main Camera, where Unity's 3D template puts it: one metre up and ten
  // back, looking along its -Z at the origin.
  {
    runtime::Transform t{};
    t.position = math::Vec3(0.0F, 1.0F, 10.0F);
    static_cast<void>(world->add_transform(cameraEntity, t));
    static_cast<void>(
        world->add_camera_component(cameraEntity, runtime::CameraComponent{}));
  }

  // Directional Light, aimed as Unity's template aims its sun (pitched 50
  // degrees down, turned 30 degrees), in its warm default colour.
  {
    runtime::Transform t{};
    t.position = math::Vec3(0.0F, 3.0F, 0.0F);
    static_cast<void>(world->add_transform(lightEntity, t));
    runtime::LightComponent sun{};
    sun.type = runtime::LightType::Directional;
    sun.color = math::Vec3(1.0F, 0.957F, 0.839F);
    sun.direction = math::Vec3(-0.321F, -0.766F, -0.557F);
    sun.intensity = 1.0F;
    static_cast<void>(world->add_light_component(lightEntity, sun));
  }

  // Scene Controller: the scene-level Lua module (assets/main.lua) the
  // project's gameplay starts from; a project without a main script (or no
  // project) gives it none.
  const char *mainScriptPath = active_config().mainScriptPath;
  if ((mainScriptPath != nullptr) && (mainScriptPath[0] != '\0')) {
    runtime::ScriptComponent sc{};
    std::snprintf(sc.behaviours[0].scriptPath,
                  sizeof(sc.behaviours[0].scriptPath), "%s", mainScriptPath);
    static_cast<void>(world->add_script_component(sceneControllerEntity, sc));
  }
}

// ---------------------------------------------------------------------------
// Scene light collection
// ---------------------------------------------------------------------------

bool runtime::catalogue_engine_content(content::AssetCatalog *catalog,
                                       const EngineConfig &config) noexcept {
  if (catalog == nullptr) {
    return false;
  }
  bool ok = true;
  // The engine's own content (shaders, the bootstrap mesh) under its own
  // mount, so its records keep their engine/... paths.
  if ((config.engineRoot != nullptr) && (config.engineRoot[0] != '\0')) {
    ok = content::register_mounted_assets(catalog, config.engineMount,
                                          config.engineRoot)
             .ok &&
         ok;
  }
  if ((config.assetRoot != nullptr) && (config.assetRoot[0] != '\0')) {
    ok = content::register_mounted_assets(catalog, config.assetMount,
                                          config.assetRoot)
             .ok &&
         ok;
    // Each package after the project, so an asset of a package that claims
    // an identity the project (or an earlier package) holds is the one
    // named, and the project's own keeps resolving.
    for (std::size_t i = 0U; i < config.packageCount; ++i) {
      ok = content::register_mounted_assets(catalog, config.packages[i].mount,
                                            config.packages[i].root)
               .ok &&
           ok;
    }
  }
  for (std::size_t i = 0U;
       i < static_cast<std::size_t>(content::BuiltinMesh::Count); ++i) {
    ok = content::register_builtin_mesh_record(
             catalog, static_cast<content::BuiltinMesh>(i)) &&
         ok;
  }
  // Once everything a document may name is catalogued (a scene names a
  // package's assets and the built-in meshes too), the documents' own
  // references become the catalog's edges. A document that will not read
  // is logged and indexed with none; loading it reports it in full.
  static_cast<void>(content::index_catalogued_documents(catalog));
  return ok;
}

} // namespace engine
