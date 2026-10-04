// Verifies the pass that gives TriMesh colliders their meshes, on the
// sample project's cooked meshes:
// - a cooked mesh builds the collision mesh of its triangles, and a file
//   that is missing, or whose cook was torn (its bytes no longer the ones
//   its cook stamp certifies), does not, with a reason;
// - a TriMesh collider naming a catalogued mesh gets it, and two colliders
//   naming one mesh share one built copy;
// - a later pass with nothing new builds and installs nothing;
// - a reference that names no mesh is reported once per World content and
//   leaves its collider without one;
// - a collider that is not a TriMesh, or names nothing, is left alone.

#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>

#include "collider_mesh_resolution.h"
#include "engine/content/asset_catalog.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/runtime/content_catalog.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

#include "../asset_root.h"
#include "../test_harness.h"

namespace {

namespace content = engine::content;
namespace physics = engine::physics;
namespace runtime = engine::runtime;

engine::tests::TestContext g_tests{};

constexpr const char *kBarrel = "assets/props/barrel.mesh";
constexpr const char *kCoin = "assets/props/coin.mesh";

runtime::Entity add_mesh_collider(runtime::World &world,
                                  const engine::core::AssetRef &ref) {
  const runtime::Entity entity =
      world.create_scene_object(runtime::Transform{});
  runtime::Collider collider{};
  collider.shape = runtime::ColliderShape::TriMesh;
  collider.meshRef = ref;
  g_tests.check((entity != runtime::kInvalidEntity) &&
                    world.add_collider(entity, collider),
                "a TriMesh collider is added");
  return entity;
}

void check_build_from_file(const std::string &assets) {
  physics::TriMeshRef mesh{};
  const char *reason = nullptr;
  g_tests.check(runtime::build_collision_mesh_from_file(
                    (assets + "/props/barrel.mesh").c_str(), &mesh, &reason) &&
                    mesh && (mesh.get()->triangle_count() > 0U),
                "a cooked mesh builds a collision mesh of its triangles");
  physics::TriMeshRef missing{};
  reason = nullptr;
  g_tests.check(
      !runtime::build_collision_mesh_from_file(
          (assets + "/props/no_such.mesh").c_str(), &missing, &reason) &&
          !missing && (reason != nullptr),
      "a missing cooked mesh builds nothing and says why");

  // A copy of the barrel's cook whose mesh bytes changed after the stamp
  // certified them, as an interrupted recook leaves it.
  const std::filesystem::path scratch = "collider_mesh_resolution_torn";
  std::error_code ec{};
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);
  for (const char *suffix : {"", ".cookmeta", ".cookstamp"}) {
    std::filesystem::copy_file(assets + "/props/barrel.mesh" + suffix,
                               scratch / (std::string("barrel.mesh") + suffix),
                               ec);
  }
  {
    std::ofstream append(scratch / "barrel.mesh",
                         std::ios::binary | std::ios::app);
    append << "torn";
  }
  physics::TriMeshRef torn{};
  reason = nullptr;
  g_tests.check(
      !ec &&
          !runtime::build_collision_mesh_from_file(
              (scratch / "barrel.mesh").string().c_str(), &torn, &reason) &&
          !torn && (reason != nullptr),
      "a cooked mesh whose bytes its stamp does not certify builds "
      "nothing and says why");
  std::filesystem::remove_all(scratch, ec);
}

void check_pass(content::AssetCatalog &catalog) {
  const content::AssetMetadata *barrel =
      content::find_asset_metadata_by_path(&catalog, kBarrel);
  const content::AssetMetadata *coin =
      content::find_asset_metadata_by_path(&catalog, kCoin);
  if ((barrel == nullptr) || (coin == nullptr) ||
      !engine::core::asset_ref_is_valid(barrel->ref) ||
      !engine::core::asset_ref_is_valid(coin->ref)) {
    g_tests.fail("the sample's barrel and coin meshes are catalogued");
    return;
  }
  std::unique_ptr<runtime::World> world(new (std::nothrow) runtime::World());
  if (world == nullptr) {
    g_tests.fail("a world is created");
    return;
  }
  world->end_frame_phase();
  const runtime::Entity first = add_mesh_collider(*world, barrel->ref);
  const runtime::Entity second = add_mesh_collider(*world, barrel->ref);
  const runtime::Entity other = add_mesh_collider(*world, coin->ref);
  engine::core::AssetRef unknown = barrel->ref;
  unknown.guid.high ^= 0xFFULL;
  const runtime::Entity broken = add_mesh_collider(*world, unknown);
  const runtime::Entity unnamed =
      add_mesh_collider(*world, engine::core::AssetRef{});
  const runtime::Entity box = world->create_scene_object(runtime::Transform{});
  g_tests.check(world->add_collider(box, runtime::Collider{}),
                "a box collider is added");

  std::unique_ptr<runtime::ColliderMeshCache> cachePtr(
      new (std::nothrow) runtime::ColliderMeshCache());
  if (cachePtr == nullptr) {
    g_tests.fail("a mesh cache is created");
    return;
  }
  runtime::ColliderMeshCache &cache = *cachePtr;
  const runtime::ColliderMeshPass pass =
      runtime::install_collider_meshes(*world, &catalog, &cache);
  g_tests.check((pass.built == 2U) && (pass.installed == 3U) &&
                    (pass.failed == 1U),
                "one pass builds the two meshes, installs three colliders "
                "and reports the broken reference");
  const physics::TriMeshData *firstMesh =
      runtime::get_tri_mesh_data(*world, first);
  g_tests.check((firstMesh != nullptr) &&
                    (runtime::get_tri_mesh_data(*world, second) == firstMesh),
                "two colliders naming one mesh share one built copy");
  g_tests.check((runtime::get_tri_mesh_data(*world, other) != nullptr) &&
                    (runtime::get_tri_mesh_data(*world, other) != firstMesh),
                "a collider naming another mesh gets that mesh");
  g_tests.check((runtime::get_tri_mesh_data(*world, broken) == nullptr) &&
                    (runtime::get_tri_mesh_data(*world, unnamed) == nullptr) &&
                    (runtime::get_tri_mesh_data(*world, box) == nullptr),
                "a broken or empty reference, or a box, gets no mesh");

  const runtime::ColliderMeshPass again =
      runtime::install_collider_meshes(*world, &catalog, &cache);
  g_tests.check((again.built == 0U) && (again.installed == 0U) &&
                    (again.failed == 0U),
                "a pass with nothing new builds, installs and reports nothing");

  // A copied world shares the meshes it was copied with.
  std::unique_ptr<runtime::World> copy(new (std::nothrow)
                                           runtime::World(*world));
  g_tests.check((copy != nullptr) &&
                    (runtime::get_tri_mesh_data(*copy, first) == firstMesh),
                "a copied world shares the built mesh");

  // New content reports the broken reference again, once.
  std::unique_ptr<runtime::World> reloaded(new (std::nothrow) runtime::World());
  if (reloaded == nullptr) {
    g_tests.fail("a second world is created");
    return;
  }
  reloaded->end_frame_phase();
  // As a scene load replaces the pipeline's world contents.
  reloaded->mark_content_replaced(world->content_epoch());
  static_cast<void>(add_mesh_collider(*reloaded, unknown));
  const runtime::Entity reused = add_mesh_collider(*reloaded, barrel->ref);
  const runtime::ColliderMeshPass fresh =
      runtime::install_collider_meshes(*reloaded, &catalog, &cache);
  g_tests.check(
      (fresh.failed == 1U) && (fresh.built == 0U) && (fresh.installed == 1U) &&
          (runtime::get_tri_mesh_data(*reloaded, reused) == firstMesh),
      "new content reuses the built mesh and reports the broken "
      "reference once more");
}

} // namespace

/// Runs the collider mesh resolution suite.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  const std::string project = engine::tests::sample_project_path();
  if (project.empty()) {
    g_tests.fail("the sample project is found");
    return g_tests.finish("collider mesh resolution");
  }
  const std::string assets =
      (std::filesystem::path(project) / "assets").string();
  check_build_from_file(assets);
  std::unique_ptr<content::AssetCatalog> catalog =
      runtime::create_asset_catalog();
  if ((catalog == nullptr) || !engine::core::mount("assets", assets.c_str())) {
    g_tests.fail("the sample's assets are mounted");
  } else {
    static_cast<void>(content::register_mounted_assets(catalog.get(), "assets",
                                                       assets.c_str()));
    check_pass(*catalog);
    static_cast<void>(engine::core::unmount("assets"));
  }
  engine::core::shutdown_logging();
  return g_tests.finish("collider mesh resolution");
}
