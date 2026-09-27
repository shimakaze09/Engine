// Verifies the Scene view's click picking. A ray hits meshes by their
// bounds under the entity's transform and colliders by their exact shape,
// nearest first with one hit per entity; a mesh box or a collider the ray
// starts inside is skipped, so a room around the camera does not swallow
// clicks; a mesh still loading is not pickable. A repeated click on the
// same spot walks to the next hit behind the current pick, wrapping, and
// any other click takes the nearest.

#include "editor_scene_query.h"

#include "engine/math/ray.h"
#include "engine/math/vec3.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

#include <array>
#include <cmath>
#include <memory>
#include <new>

namespace {

using engine::editor::PickHit;
using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;

constexpr std::uint64_t kUnitMesh = 1U;
constexpr std::uint64_t kLoadingMesh = 2U;

/// Mesh 1 is a unit cube (half extents 0.5) about its origin; every other
/// mesh is still loading.
bool fake_mesh_bounds(std::uint64_t id, Vec3 *center, Vec3 *half) noexcept {
  if (id != kUnitMesh) {
    return false;
  }
  *center = Vec3();
  *half = Vec3(0.5F, 0.5F, 0.5F);
  return true;
}

Entity place(World &world, const Vec3 &position, float scale = 1.0F) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  transform.scale = Vec3(scale, scale, scale);
  return world.create_scene_object(transform);
}

bool add_mesh(World &world, Entity entity, std::uint64_t id) noexcept {
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = id;
  return world.add_mesh_component(entity, mesh);
}

bool add_box_collider(World &world, Entity entity, float half) noexcept {
  engine::runtime::Collider collider{};
  collider.halfExtents = Vec3(half, half, half);
  return world.add_collider(entity, collider);
}

// Slab hits on unit-cube faces at distances of order 10: exact in float
// for these axis-aligned rays, but held to a few ulps.
bool near(float a, float b) noexcept { return std::fabs(a - b) <= 1.0e-5F; }

void check_hits(engine::tests::TestContext &t, World &world) noexcept {
  // Down -z from the origin: a mesh cube at depth 5 (front face 4.5), a
  // collider box of half 1 at depth 10 (front face 9), and one entity with
  // both a mesh (front face 14.5) and a larger collider (front face 13)
  // at depth 15.
  const Entity nearMesh = place(world, Vec3(0.0F, 0.0F, -5.0F));
  const Entity midCollider = place(world, Vec3(0.0F, 0.0F, -10.0F));
  const Entity both = place(world, Vec3(0.0F, 0.0F, -15.0F));
  t.check(add_mesh(world, nearMesh, kUnitMesh) &&
              add_box_collider(world, midCollider, 1.0F) &&
              add_mesh(world, both, kUnitMesh) &&
              add_box_collider(world, both, 2.0F),
          "build the row of targets");
  // A room around the camera, both as a mesh and as a collider, and a
  // mesh still loading in the line of sight.
  const Entity roomMesh = place(world, Vec3(), 100.0F);
  const Entity roomCollider = place(world, Vec3());
  const Entity loading = place(world, Vec3(0.0F, 0.0F, -2.0F));
  t.check(add_mesh(world, roomMesh, kUnitMesh) &&
              add_box_collider(world, roomCollider, 50.0F) &&
              add_mesh(world, loading, kLoadingMesh),
          "build the room and the loading mesh");

  engine::math::Ray ray{};
  ray.origin = Vec3();
  ray.direction = Vec3(0.0F, 0.0F, -1.0F);
  std::array<PickHit, 8> hits{};
  const std::size_t count = engine::editor::scene_pick_hits(
      world, ray, 100.0F, &fake_mesh_bounds, hits.data(), hits.size());
  t.check(count == 3U, "three pickable things lie on the ray");
  t.check((hits[0].entity == nearMesh) && near(hits[0].distance, 4.5F),
          "the nearest is the mesh, at its bounds' face");
  t.check((hits[1].entity == midCollider) && near(hits[1].distance, 9.0F),
          "then the collider, at its shape's face");
  t.check((hits[2].entity == both) && near(hits[2].distance, 13.0F),
          "an entity with both is listed once, at its nearer hit");

  std::array<PickHit, 2> two{};
  t.check(
      (engine::editor::scene_pick_hits(world, ray, 100.0F, &fake_mesh_bounds,
                                       two.data(), two.size()) == 2U) &&
          (two[0].entity == nearMesh) && (two[1].entity == midCollider),
      "a short list keeps the nearest");
  t.check(engine::editor::scene_pick_hits(world, ray, 4.0F, &fake_mesh_bounds,
                                          hits.data(), hits.size()) == 0U,
          "nothing within reach, nothing hit");
  ray.direction = Vec3(0.0F, 1.0F, 0.0F);
  t.check(engine::editor::scene_pick_hits(world, ray, 100.0F, &fake_mesh_bounds,
                                          hits.data(), hits.size()) == 0U,
          "a ray into empty space hits nothing but the room it starts in");
}

void check_cycle(engine::tests::TestContext &t) noexcept {
  const PickHit hits[3] = {
      {Entity{1U, 1U}, 1.0F}, {Entity{2U, 1U}, 2.0F}, {Entity{3U, 1U}, 3.0F}};
  using engine::editor::choose_pick;
  t.check(choose_pick(hits, 3U, kInvalidEntity, false) == hits[0].entity,
          "a click takes the nearest");
  t.check(choose_pick(hits, 3U, hits[0].entity, true) == hits[1].entity,
          "a repeat click on the same spot takes the next behind");
  t.check(choose_pick(hits, 3U, hits[2].entity, true) == hits[0].entity,
          "past the farthest it wraps to the nearest");
  t.check(choose_pick(hits, 3U, hits[1].entity, false) == hits[0].entity,
          "a click elsewhere takes the nearest again");
  t.check(choose_pick(hits, 3U, Entity{9U, 1U}, true) == hits[0].entity,
          "a pick that is not under the cursor starts from the nearest");
  t.check(choose_pick(hits, 0U, kInvalidEntity, true) == kInvalidEntity,
          "no hits, no pick");
}

} // namespace

int main() {
  engine::tests::TestContext t;
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 99;
  }
  check_hits(t, *world);
  check_cycle(t);
  return t.finish("editor_scene_query");
}
