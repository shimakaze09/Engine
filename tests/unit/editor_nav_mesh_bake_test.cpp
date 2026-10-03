// Verifies the editor's NavMeshSurface Bake through its production entry
// point, with the asset mount pointed at a scratch directory:
// - a surface with no path is baked to a name chosen under the mount, and
//   a second one to a numbered name beside it, never over the first;
// - the written file gets a sidecar identity, which a rebake keeps;
// - the file decodes to the mesh the bake made;
// - a volume with nothing walkable, or a path to another kind of file,
//   writes nothing and keeps the previous file.

#include "editor_nav_mesh_bake.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "../test_harness.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/vfs.h"
#include "engine/runtime/navigation_bake.h"
#include "engine/runtime/scene_navigation.h"

namespace {

using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::NavMeshSurfaceComponent;
using engine::runtime::World;

engine::tests::TestContext g_tests;

constexpr const char *kDirectory = "editor_nav_mesh_bake_test_files";

/// A World with a 20 by 20 m static floor whose top is at y = 0.
std::unique_ptr<World> make_level() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return world;
  }
  world->end_frame_phase();
  const Entity floor = world->create_entity();
  engine::runtime::Transform transform{};
  transform.position = Vec3(0.0F, -0.5F, 0.0F);
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(10.0F, 0.5F, 10.0F);
  static_cast<void>(world->add_transform(floor, transform));
  static_cast<void>(world->add_collider(floor, collider));
  return world;
}

/// An entity at `position` with a surface over the floor and no path.
Entity add_surface(World &world, const Vec3 &position) noexcept {
  const Entity entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = position;
  NavMeshSurfaceComponent surface{};
  surface.halfExtents = Vec3(10.0F, 2.0F, 10.0F);
  surface.cellSize = 0.5F;
  static_cast<void>(world.add_transform(entity, transform));
  static_cast<void>(world.add_nav_mesh_surface(entity, surface));
  return entity;
}

/// The sidecar identity beside the mounted `path`; false when none reads.
bool sidecar_of(const char *path, engine::content::AssetSidecar *out) {
  char osPath[512] = {};
  return engine::core::vfs_resolve_os_path(path, osPath, sizeof(osPath)) &&
         (engine::content::read_asset_sidecar(osPath, out) ==
          engine::content::SidecarReadResult::Ok);
}

/// The content hash of the mesh the file at `path` decodes to; 0 when it
/// does not.
std::uint64_t file_hash(const char *path) noexcept {
  engine::navigation::NavMesh mesh{};
  return engine::runtime::load_nav_mesh_file(path, &mesh) ? mesh.content_hash()
                                                          : 0U;
}

void test_bake() {
  std::unique_ptr<World> world = make_level();
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  const Entity first = add_surface(*world, Vec3(0.0F, 0.0F, 0.0F));
  const Entity second = add_surface(*world, Vec3(0.0F, 0.0F, 0.0F));

  char firstPath[NavMeshSurfaceComponent::kMaxPathLength + 1U] = {};
  const engine::editor::NavMeshBakeReport baked =
      engine::editor::bake_nav_mesh_surface_file(*world, first, firstPath,
                                                 sizeof(firstPath));
  g_tests.check(baked.written, "a surface with no path bakes");
  g_tests.check(std::strcmp(firstPath, "assets/NavMesh.navmesh") == 0,
                "an unsaved scene's first bake is named NavMesh");

  char secondPath[NavMeshSurfaceComponent::kMaxPathLength + 1U] = {};
  g_tests.check(engine::editor::bake_nav_mesh_surface_file(
                    *world, second, secondPath, sizeof(secondPath))
                        .written &&
                    (std::strcmp(secondPath, "assets/NavMesh_2.navmesh") == 0),
                "a second surface is baked beside the first, not over it");

  engine::navigation::NavMesh expected{};
  g_tests.check(
      engine::runtime::bake_nav_mesh_surface(*world, first, &expected) &&
          (file_hash(firstPath) == expected.content_hash()),
      "the file holds the mesh the bake made");

  engine::content::AssetSidecar identity{};
  g_tests.check(sidecar_of(firstPath, &identity),
                "the baked file has a sidecar identity");
  g_tests.check(engine::editor::bake_nav_mesh_surface_file(
                    *world, first, firstPath, sizeof(firstPath))
                    .written,
                "a rebake to the same path writes");
  engine::content::AssetSidecar rebakedIdentity{};
  g_tests.check(sidecar_of(firstPath, &rebakedIdentity) &&
                    (rebakedIdentity.guid == identity.guid),
                "a rebake keeps the file's identity");

  // Moved off the floor, the volume holds nothing walkable.
  const Entity away = add_surface(*world, Vec3(0.0F, 50.0F, 0.0F));
  char awayPath[NavMeshSurfaceComponent::kMaxPathLength + 1U] = {};
  std::snprintf(awayPath, sizeof(awayPath), "%s", firstPath);
  const std::uint64_t before = file_hash(firstPath);
  const engine::editor::NavMeshBakeReport empty =
      engine::editor::bake_nav_mesh_surface_file(*world, away, awayPath,
                                                 sizeof(awayPath));
  g_tests.check(!empty.written && (file_hash(firstPath) == before) &&
                    (before != 0U),
                "an empty volume writes nothing and keeps the file");

  char wrongKind[NavMeshSurfaceComponent::kMaxPathLength + 1U] =
      "assets/level.scene";
  g_tests.check(!engine::editor::bake_nav_mesh_surface_file(
                     *world, first, wrongKind, sizeof(wrongKind))
                        .written &&
                    !engine::core::vfs_file_exists("assets/level.scene"),
                "a path to another kind of file writes nothing");

  char tooSmall[16] = {};
  g_tests.check(!engine::editor::bake_nav_mesh_surface_file(
                     *world, first, tooSmall, sizeof(tooSmall))
                     .written,
                "a buffer that cannot hold a surface path is refused");
}

} // namespace

/// Runs the editor navigation bake suite.
int main() {
  std::error_code ec{};
  std::filesystem::remove_all(kDirectory, ec);
  std::filesystem::create_directories(kDirectory, ec);
  if (!engine::core::initialize_vfs() ||
      !engine::core::mount("assets", kDirectory)) {
    std::fprintf(stderr, "editor_nav_mesh_bake_test: could not mount %s\n",
                 kDirectory);
    return 1;
  }
  test_bake();
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(kDirectory, ec);
  return g_tests.finish("editor_nav_mesh_bake_test");
}
