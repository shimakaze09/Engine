// Helper for engine_integration_engine_validate_navmesh: writes the
// fixture scene that test bakes, and runs the editor's own NavMeshSurface
// Bake on a saved scene, so the test can hold engine_validate's
// --bake-navmesh output to the file the editor's Bake button writes.
//
//   editor_nav_mesh_bake_probe write-scene <file.scene> <floor top>
//     A 20 by 20 m static floor whose top is at <floor top> and a surface
//     over it whose file is assets/level.navmesh, with the scene's sidecar
//     identity.
//   editor_nav_mesh_bake_probe editor-bake <assets dir> <file.scene> <path>
//     Loads the scene with <assets dir> mounted as assets and bakes its
//     surface to the mounted <path> through editor::bake_nav_mesh_surface_file.

#include "editor_nav_mesh_bake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/reflect_types.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::World;

int write_scene(const char *scenePath, float floorTop) {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 2;
  }
  const Entity floor = world->create_entity();
  engine::runtime::Transform floorTransform{};
  floorTransform.position = Vec3(0.0F, floorTop - 0.5F, 0.0F);
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(10.0F, 0.5F, 10.0F);
  const Entity surfaceEntity = world->create_entity();
  engine::runtime::NavMeshSurfaceComponent surface{};
  surface.halfExtents = Vec3(10.0F, 3.0F, 10.0F);
  surface.cellSize = 0.5F;
  std::snprintf(surface.navMeshPath, sizeof(surface.navMeshPath), "%s",
                "assets/level.navmesh");
  if (!world->add_transform(floor, floorTransform) ||
      !world->add_collider(floor, collider) ||
      !world->add_transform(surfaceEntity, engine::runtime::Transform{}) ||
      !world->add_nav_mesh_surface(surfaceEntity, surface) ||
      !engine::runtime::save_scene(*world, scenePath) ||
      // The scene gets the sidecar identity the editor's save gives it, so
      // the catalog engine_validate takes indexes cleanly.
      (engine::runtime::editor_establish_asset_identity(scenePath) ==
       engine::runtime::EditorIdentityResult::WriteFailed)) {
    std::fprintf(stderr, "probe: could not write %s\n", scenePath);
    return 3;
  }
  return 0;
}

int editor_bake(const char *assetsDirectory, const char *scenePath,
                const char *outPath) {
  if (!engine::core::initialize_vfs() ||
      !engine::core::mount("assets", assetsDirectory)) {
    std::fprintf(stderr, "probe: could not mount %s\n", assetsDirectory);
    return 2;
  }
  std::unique_ptr<World> world(new (std::nothrow) World());
  if ((world == nullptr) ||
      !engine::runtime::load_scene(*world, scenePath, nullptr, nullptr) ||
      (world->nav_mesh_surface_count() == 0U)) {
    std::fprintf(stderr, "probe: could not load a surface from %s\n",
                 scenePath);
    return 3;
  }
  // The editor bakes over a World its frames have propagated.
  world->begin_transform_phase();
  world->end_frame_phase();
  char path[engine::runtime::NavMeshSurfaceComponent::kMaxPathLength + 1U] =
      {};
  std::snprintf(path, sizeof(path), "%s", outPath);
  const engine::editor::NavMeshBakeReport report =
      engine::editor::bake_nav_mesh_surface_file(
          *world, world->nav_mesh_surface_entity_at(0U), path, sizeof(path));
  std::printf("probe: %s\n", report.message);
  engine::core::shutdown_vfs();
  return report.written ? 0 : 4;
}

} // namespace

/// Runs the probe; see the file comment for its two commands.
int main(int argc, char **argv) {
  engine::runtime::ensure_runtime_reflection_registered();
  static_cast<void>(engine::core::initialize_logging());
  int result = 1;
  if ((argc == 4) && (std::strcmp(argv[1], "write-scene") == 0)) {
    result = write_scene(argv[2], static_cast<float>(std::atof(argv[3])));
  } else if ((argc == 5) && (std::strcmp(argv[1], "editor-bake") == 0)) {
    result = editor_bake(argv[2], argv[3], argv[4]);
  } else {
    std::fprintf(stderr, "usage: editor_nav_mesh_bake_probe write-scene "
                         "<file.scene> <floor top> | editor-bake <assets "
                         "dir> <file.scene> <path>\n");
  }
  engine::core::shutdown_logging();
  return result;
}
