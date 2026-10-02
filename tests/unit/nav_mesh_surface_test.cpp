// Verifies the NavMeshSurface component and the scene's navigation meshes:
// - a surface whose volume cannot bake, or whose path names no .navmesh
//   file, is refused by the World with the entity unchanged;
// - a scene round-trips a surface through the production serializer, and
//   a scene whose surface carries a mistyped field, a path that does not
//   fit whole or a path to another kind of file is refused with the World
//   unchanged; a path to no file loads and is reported missing_nav_mesh;
// - a surface baked, written and read back through SceneNavigation holds
//   the mesh that was baked, by content hash;
// - a missing or damaged file leaves the surface without a mesh, a new
//   path is read at once, and a rewritten file is read again only when a
//   reload is requested.

#include "engine/runtime/scene_navigation.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "engine/core/validation_report.h"
#include "engine/core/vfs.h"
#include "engine/runtime/navigation_bake.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

namespace nav = engine::navigation;
using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::NavMeshSurfaceComponent;
using engine::runtime::World;

engine::tests::TestContext g_tests;

constexpr const char *kMount = "navtest";
constexpr const char *kDirectory = "nav_mesh_surface_test_files";

std::unique_ptr<World> make_world() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world != nullptr) {
    world->end_frame_phase();
  }
  return world;
}

/// A 20 by 20 m static floor whose top is at y = 0.
void add_floor(World &world) noexcept {
  const Entity floor = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = Vec3(0.0F, -0.5F, 0.0F);
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(10.0F, 0.5F, 10.0F);
  static_cast<void>(world.add_transform(floor, transform));
  static_cast<void>(world.add_collider(floor, collider));
}

/// An entity at the origin carrying `surface`; kInvalidEntity when the
/// World refuses it.
Entity add_surface(World &world, const NavMeshSurfaceComponent &surface) {
  const Entity entity = world.create_entity();
  static_cast<void>(world.add_transform(entity, engine::runtime::Transform{}));
  return world.add_nav_mesh_surface(entity, surface)
             ? entity
             : engine::runtime::kInvalidEntity;
}

NavMeshSurfaceComponent surface_at(const char *path) noexcept {
  NavMeshSurfaceComponent surface{};
  surface.halfExtents = Vec3(10.0F, 2.0F, 10.0F);
  surface.cellSize = 0.5F;
  std::snprintf(surface.navMeshPath, sizeof(surface.navMeshPath), "%s", path);
  return surface;
}

/// Bakes `surface` over `world` and writes it at its path; the mesh's
/// content hash, or 0 when any step failed.
std::uint64_t bake_and_write(const World &world, Entity surface,
                             const char *path) noexcept {
  nav::NavMesh mesh{};
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  if (!engine::runtime::bake_nav_mesh_surface(world, surface, &mesh) ||
      mesh.empty() || !nav::write_nav_mesh(mesh, &bytes, &size) ||
      !engine::core::vfs_write_binary(path, bytes.get(), size)) {
    return 0U;
  }
  return mesh.content_hash();
}

void test_validation() {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  const NavMeshSurfaceComponent defaults{};
  g_tests.check(engine::runtime::nav_mesh_surface_is_valid(defaults),
                "the default surface can bake");

  const Entity entity = world->create_entity();
  NavMeshSurfaceComponent bad{};
  bad.halfExtents.y = 0.0F;
  g_tests.check(!world->add_nav_mesh_surface(entity, bad),
                "a volume with no height is refused");
  bad = NavMeshSurfaceComponent{};
  bad.halfExtents.x = std::numeric_limits<float>::quiet_NaN();
  g_tests.check(!world->add_nav_mesh_surface(entity, bad),
                "a volume of NaN size is refused");
  // 1000 m by 1000 m in quarter-metre columns is 16 million columns, past
  // the most one bake samples.
  bad = NavMeshSurfaceComponent{};
  bad.halfExtents = Vec3(500.0F, 10.0F, 500.0F);
  g_tests.check(!world->add_nav_mesh_surface(entity, bad),
                "a volume of more columns than a bake samples is refused");
  bad = NavMeshSurfaceComponent{};
  bad.maxSlopeDegrees = 95.0F;
  g_tests.check(!world->add_nav_mesh_surface(entity, bad),
                "a slope past 89 degrees is refused");
  bad = surface_at("assets/level.scene");
  g_tests.check(!world->add_nav_mesh_surface(entity, bad),
                "a path to another kind of file is refused");
  g_tests.check(!world->has_nav_mesh_surface(entity),
                "every refusal left the entity without a surface");

  const NavMeshSurfaceComponent good = surface_at("assets/Level.NAVMESH");
  g_tests.check(world->add_nav_mesh_surface(entity, good),
                "a .navmesh path in any case is accepted");
  NavMeshSurfaceComponent stored{};
  g_tests.check(world->get_nav_mesh_surface(entity, &stored) &&
                    (std::strcmp(stored.navMeshPath, good.navMeshPath) == 0),
                "the World holds the surface it was given");
  g_tests.check(!world->add_nav_mesh_surface(entity, bad) &&
                    world->get_nav_mesh_surface(entity, &stored) &&
                    (std::strcmp(stored.navMeshPath, good.navMeshPath) == 0),
                "a refused replacement keeps the surface it had");
}

/// The saved document of a World holding one surface.
std::string saved_scene(const NavMeshSurfaceComponent &surface) {
  std::unique_ptr<World> world = make_world();
  if ((world == nullptr) ||
      (add_surface(*world, surface) == engine::runtime::kInvalidEntity)) {
    return {};
  }
  std::unique_ptr<char[]> buffer{};
  std::size_t size = 0U;
  if (!engine::runtime::save_scene(*world, &buffer, &size)) {
    return {};
  }
  return std::string(buffer.get(), size);
}

/// True when loading `document` into a World holding one plain entity is
/// refused and leaves that entity in place. An empty document, which a
/// failed replacement leaves, is not a refusal this suite means to test.
bool load_refused(const std::string &document) {
  std::unique_ptr<World> world = make_world();
  if ((world == nullptr) || document.empty()) {
    return false;
  }
  const Entity before = world->create_entity();
  const bool loaded =
      engine::runtime::load_scene(*world, document.data(), document.size());
  return !loaded && world->is_alive(before);
}

/// `document` with the first `from` replaced by `to`; empty when absent.
std::string replaced(std::string document, const std::string &from,
                     const std::string &to) {
  const std::size_t at = document.find(from);
  if (at == std::string::npos) {
    return {};
  }
  return document.replace(at, from.size(), to);
}

void test_scene_round_trip() {
  const NavMeshSurfaceComponent surface = surface_at("assets/arena.navmesh");
  const std::string document = saved_scene(surface);
  g_tests.check(!document.empty(), "a scene with a surface saves");

  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  engine::core::ValidationReport report{};
  g_tests.check(engine::runtime::load_scene(*world, document.data(),
                                            document.size(), nullptr, &report),
                "the saved scene loads");
  g_tests.check(world->nav_mesh_surface_count() == 1U,
                "the loaded scene holds its surface");
  const NavMeshSurfaceComponent *loaded = world->nav_mesh_surface_at(0U);
  g_tests.check(
      (loaded != nullptr) && (loaded->halfExtents.x == surface.halfExtents.x) &&
          (loaded->halfExtents.y == surface.halfExtents.y) &&
          (loaded->cellSize == surface.cellSize) &&
          (std::strcmp(loaded->navMeshPath, surface.navMeshPath) == 0),
      "the surface loads as it was saved");
  std::unique_ptr<char[]> again{};
  std::size_t againSize = 0U;
  g_tests.check(engine::runtime::save_scene(*world, &again, &againSize) &&
                    (std::string(again.get(), againSize) == document),
                "saving the loaded scene writes the same bytes");

  g_tests.check(load_refused(replaced(document, "\"cellSize\":0.5",
                                      "\"cellSize\":\"half\"")),
                "a cell size that is not a number is refused");
  g_tests.check(
      load_refused(replaced(document, "assets/arena.navmesh",
                            "assets/" + std::string(140U, 'a') + ".navmesh")),
      "a path longer than the field is refused, not cut");
  g_tests.check(load_refused(replaced(document, "assets/arena.navmesh",
                                      "assets/arena.lua")),
                "a path to a script is refused");
  g_tests.check(load_refused(replaced(document, "\"maxSlopeDegrees\":45",
                                      "\"maxSlopeDegrees\":120")),
                "a slope the bake refuses is refused at load");
}

void test_missing_reported() {
  const std::string document =
      saved_scene(surface_at("navtest/never_baked.navmesh"));
  std::unique_ptr<World> world = make_world();
  if ((world == nullptr) || document.empty()) {
    g_tests.check(false, "build the scene");
    return;
  }
  engine::core::ValidationReport report{};
  g_tests.check(engine::runtime::load_scene(*world, document.data(),
                                            document.size(), nullptr, &report),
                "a scene naming an unbaked file still loads");
  bool reported = false;
  for (std::size_t i = 0U; i < report.count; ++i) {
    reported = reported ||
               (std::strcmp(report.entries[i].code, "missing_nav_mesh") == 0);
  }
  g_tests.check(reported, "the unbaked file is reported missing_nav_mesh");
}

void test_bake_then_load() {
  std::unique_ptr<World> world = make_world();
  if (world == nullptr) {
    g_tests.check(false, "allocate a World");
    return;
  }
  add_floor(*world);
  const char *path = "navtest/floor.navmesh";
  const Entity surface = add_surface(*world, surface_at(path));
  g_tests.check(surface != engine::runtime::kInvalidEntity,
                "the World takes the surface");
  const std::uint64_t baked = bake_and_write(*world, surface, path);
  g_tests.check(baked != 0U, "the floor bakes and the file is written");

  engine::runtime::SceneNavigation navigation{};
  navigation.update(*world);
  const nav::NavMesh *loaded = navigation.mesh_for(surface);
  g_tests.check((navigation.count() == 1U) && (loaded != nullptr) &&
                    (loaded->content_hash() == baked),
                "the scene reads the mesh that was baked");

  // A rebake with coarser columns is a different mesh; the file changes,
  // but nothing is read until a bake asks.
  NavMeshSurfaceComponent coarser = surface_at(path);
  coarser.cellSize = 1.0F;
  static_cast<void>(world->add_nav_mesh_surface(surface, coarser));
  const std::uint64_t rebaked = bake_and_write(*world, surface, path);
  g_tests.check((rebaked != 0U) && (rebaked != baked),
                "the coarser bake is a different mesh");
  navigation.update(*world);
  loaded = navigation.mesh_for(surface);
  g_tests.check((loaded != nullptr) && (loaded->content_hash() == baked),
                "an unchanged surface reads nothing again");
  engine::runtime::request_scene_navigation_reload();
  navigation.update(*world);
  loaded = navigation.mesh_for(surface);
  g_tests.check((loaded != nullptr) && (loaded->content_hash() == rebaked),
                "a requested reload reads the rewritten file");

  // A new path is read at once; a missing file leaves no mesh.
  static_cast<void>(world->add_nav_mesh_surface(
      surface, surface_at("navtest/nothing_here.navmesh")));
  navigation.update(*world);
  g_tests.check(navigation.mesh_for(surface) == nullptr,
                "a surface whose file is missing has no mesh");

  const char garbage[] = "not a navigation mesh";
  g_tests.check(engine::core::vfs_write_binary("navtest/damaged.navmesh",
                                               garbage, sizeof(garbage)),
                "write a damaged file");
  static_cast<void>(world->add_nav_mesh_surface(
      surface, surface_at("navtest/damaged.navmesh")));
  navigation.update(*world);
  g_tests.check(navigation.mesh_for(surface) == nullptr,
                "a surface whose file is damaged has no mesh");

  static_cast<void>(world->add_nav_mesh_surface(surface, surface_at(path)));
  navigation.update(*world);
  loaded = navigation.mesh_for(surface);
  g_tests.check((loaded != nullptr) && (loaded->content_hash() == rebaked),
                "going back to the baked file reads it");

  static_cast<void>(world->remove_nav_mesh_surface(surface));
  navigation.update(*world);
  g_tests.check((navigation.count() == 0U) &&
                    (navigation.mesh_for(surface) == nullptr),
                "a removed surface's mesh is forgotten");
}

} // namespace

/// Runs the navigation surface suites.
int main() {
  std::error_code ec{};
  std::filesystem::remove_all(kDirectory, ec);
  std::filesystem::create_directories(kDirectory, ec);
  if (!engine::core::initialize_vfs() ||
      !engine::core::mount(kMount, kDirectory)) {
    std::fprintf(stderr, "nav_mesh_surface_test: could not mount %s\n",
                 kDirectory);
    return 1;
  }
  test_validation();
  test_scene_round_trip();
  test_missing_reported();
  test_bake_then_load();
  engine::core::shutdown_vfs();
  std::filesystem::remove_all(kDirectory, ec);
  return g_tests.finish("nav_mesh_surface_test");
}
