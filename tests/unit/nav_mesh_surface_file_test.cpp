// Verifies the shared NavMeshSurface file write and staleness check that
// the editor's Bake button and engine_validate run, with the asset mount
// pointed at a scratch directory:
// - a write produces the bytes a bake encodes, with a sidecar identity
//   that a rebake keeps;
// - an empty volume, a failed bake or a path to another kind of file
//   writes nothing and keeps the previous file;
// - the check reads Current for the file just written, Stale once the
//   floor moves or the file's bytes change, Missing for no file, and says
//   when there is nothing to compare.

#include "engine/runtime/nav_mesh_surface_file.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "../test_harness.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/vfs.h"
#include "engine/navigation/nav_mesh.h"
#include "engine/runtime/navigation_bake.h"

namespace {

using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::NavMeshFileState;
using engine::runtime::NavMeshWriteResult;
using engine::runtime::NavMeshSurfaceComponent;
using engine::runtime::World;

engine::tests::TestContext g_tests;

constexpr const char *kDirectory = "nav_mesh_surface_file_test_files";
constexpr const char *kPath = "assets/level.navmesh";

/// Places a 20 by 20 m static floor on `floor` with its top at `top`.
void place_floor(World &world, Entity floor, float top) noexcept {
  engine::runtime::Transform transform{};
  transform.position = Vec3(0.0F, top - 0.5F, 0.0F);
  static_cast<void>(world.add_transform(floor, transform));
}

/// A World with that floor, its top at y = 0; *floorOut names it.
std::unique_ptr<World> make_level(Entity *floorOut) noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return world;
  }
  world->end_frame_phase();
  const Entity floor = world->create_entity();
  place_floor(*world, floor, 0.0F);
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(10.0F, 0.5F, 10.0F);
  static_cast<void>(world->add_collider(floor, collider));
  *floorOut = floor;
  return world;
}

/// An entity at `position` with a surface over the floor.
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

/// The bytes of the mounted file at `path`; false when it does not read.
bool file_bytes(const char *path, std::unique_ptr<std::uint8_t[]> *out,
                std::size_t *size) {
  void *data = nullptr;
  if (!engine::core::vfs_read_binary(path, &data, size).succeeded()) {
    return false;
  }
  out->reset(new (std::nothrow) std::uint8_t[*size]);
  const bool copied = (*out != nullptr);
  if (copied) {
    std::memcpy(out->get(), data, *size);
  }
  engine::core::vfs_free(data);
  return copied;
}

/// True when the file at `path` holds exactly the encoding of `entity`'s
/// bake now.
bool file_is_bake_of(const World &world, Entity entity, const char *path) {
  engine::navigation::NavMesh mesh{};
  std::unique_ptr<std::uint8_t[]> expected{};
  std::size_t expectedSize = 0U;
  std::unique_ptr<std::uint8_t[]> actual{};
  std::size_t actualSize = 0U;
  return engine::runtime::bake_nav_mesh_surface(world, entity, &mesh) &&
         engine::navigation::write_nav_mesh(mesh, &expected, &expectedSize) &&
         file_bytes(path, &actual, &actualSize) &&
         (actualSize == expectedSize) &&
         (std::memcmp(actual.get(), expected.get(), actualSize) == 0);
}

void test_write() {
  Entity floor{};
  std::unique_ptr<World> world = make_level(&floor);
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  const Entity surface = add_surface(*world, Vec3(0.0F, 0.0F, 0.0F));

  const engine::runtime::NavMeshWriteReport written =
      engine::runtime::write_nav_mesh_surface_file(*world, surface, kPath);
  g_tests.check(written.result == NavMeshWriteResult::Written &&
                    (written.polygons > 0U) && written.identified,
                "a surface over the floor is written with an identity");
  g_tests.check(file_is_bake_of(*world, surface, kPath),
                "the file holds exactly the bytes the bake encodes");

  engine::content::AssetSidecar identity{};
  engine::content::AssetSidecar rebaked{};
  g_tests.check(sidecar_of(kPath, &identity) &&
                    (engine::runtime::write_nav_mesh_surface_file(
                         *world, surface, kPath)
                         .result == NavMeshWriteResult::Written) &&
                    sidecar_of(kPath, &rebaked) &&
                    (rebaked.guid == identity.guid),
                "a rebake keeps the file's identity");

  // Raised off the floor, the volume holds nothing walkable.
  const Entity away = add_surface(*world, Vec3(0.0F, 50.0F, 0.0F));
  g_tests.check(
      (engine::runtime::write_nav_mesh_surface_file(*world, away, kPath)
           .result == NavMeshWriteResult::Empty) &&
          file_is_bake_of(*world, surface, kPath),
      "an empty volume writes nothing and keeps the file");
  g_tests.check(engine::runtime::write_nav_mesh_surface_file(*world, floor,
                                                             kPath)
                        .result == NavMeshWriteResult::BakeFailed,
                "an entity with no surface is refused");
  g_tests.check(
      (engine::runtime::write_nav_mesh_surface_file(*world, surface,
                                                    "assets/level.scene")
           .result == NavMeshWriteResult::BadPath) &&
          !engine::core::vfs_file_exists("assets/level.scene"),
      "a path to another kind of file writes nothing");
  g_tests.check(
      engine::runtime::write_nav_mesh_surface_file(*world, surface, "").result ==
          NavMeshWriteResult::BadPath,
      "an empty path is refused");
}

void test_check() {
  Entity floor{};
  std::unique_ptr<World> world = make_level(&floor);
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  const Entity surface = add_surface(*world, Vec3(0.0F, 0.0F, 0.0F));
  using engine::runtime::check_nav_mesh_surface_file;

  g_tests.check(check_nav_mesh_surface_file(*world, surface,
                                            "assets/never.navmesh") ==
                    NavMeshFileState::Missing,
                "no file reads Missing");
  g_tests.check(check_nav_mesh_surface_file(*world, surface,
                                            "assets/level.scene") ==
                    NavMeshFileState::BadPath,
                "a path to another kind of file reads BadPath");

  g_tests.check(
      (engine::runtime::write_nav_mesh_surface_file(*world, surface, kPath)
           .result == NavMeshWriteResult::Written) &&
          (check_nav_mesh_surface_file(*world, surface, kPath) ==
           NavMeshFileState::Current),
      "the file just written reads Current");

  // The floor rises half a metre: the level the file describes is gone.
  place_floor(*world, floor, 0.5F);
  g_tests.check(check_nav_mesh_surface_file(*world, surface, kPath) ==
                    NavMeshFileState::Stale,
                "a moved floor reads Stale");
  place_floor(*world, floor, 0.0F);
  g_tests.check(check_nav_mesh_surface_file(*world, surface, kPath) ==
                    NavMeshFileState::Current,
                "the floor put back reads Current again");

  // Same length, one byte different: a file not written by this bake.
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  const bool read = file_bytes(kPath, &bytes, &size) && (size > 16U);
  if (read) {
    bytes[16U] = static_cast<std::uint8_t>(bytes[16U] ^ 0x01U);
  }
  g_tests.check(read &&
                    engine::core::vfs_write_binary(kPath, bytes.get(), size) &&
                    (check_nav_mesh_surface_file(*world, surface, kPath) ==
                     NavMeshFileState::Stale),
                "a file whose bytes differ reads Stale");

  const Entity away = add_surface(*world, Vec3(0.0F, 50.0F, 0.0F));
  g_tests.check(check_nav_mesh_surface_file(*world, away, kPath) ==
                    NavMeshFileState::BakeFailed,
                "a volume with nothing walkable has nothing to compare");
}

} // namespace

/// Runs the NavMeshSurface file suite.
int main() {
  std::error_code ec{};
  std::filesystem::remove_all(kDirectory, ec);
  std::filesystem::create_directories(kDirectory, ec);
  if (!engine::core::initialize_vfs() ||
      !engine::core::mount("assets", kDirectory)) {
    std::fprintf(stderr, "nav_mesh_surface_file_test: could not mount %s\n",
                 kDirectory);
    return 1;
  }
  test_write();
  test_check();
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(kDirectory, ec);
  return g_tests.finish("nav_mesh_surface_file_test");
}
