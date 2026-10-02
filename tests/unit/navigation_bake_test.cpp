// Verifies the navigation mesh bake over a real World through the
// production physics ray queries: static colliders (with no body, and with
// a Static one) are the level, while a dynamic crate and a trigger are not;
// a path from one side of a wall to the other goes around its end; a
// platform within the climb is reached at its own height; and the same
// World bakes to the same mesh twice.

#include "engine/runtime/navigation_bake.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/runtime/world.h"

namespace {

namespace nav = engine::navigation;
using engine::math::Vec3;
using engine::runtime::Entity;
using engine::runtime::World;

engine::tests::TestContext g_tests;

/// A box collider at `position`, with a rigid body of `bodyType` unless
/// `bodyType` is negative, and a trigger when asked.
Entity add_box(World &world, const Vec3 &position, const Vec3 &halfExtents,
               int bodyType = -1, bool isTrigger = false) noexcept {
  const Entity entity = world.create_entity();
  engine::runtime::Transform transform{};
  transform.position = position;
  engine::runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  collider.isTrigger = isTrigger;
  static_cast<void>(world.add_transform(entity, transform));
  static_cast<void>(world.add_collider(entity, collider));
  if (bodyType >= 0) {
    engine::runtime::RigidBody body{};
    body.bodyType = static_cast<std::uint32_t>(bodyType);
    body.inverseMass = (bodyType == 0) ? 1.0F : 0.0F;
    static_cast<void>(world.add_rigid_body(entity, body));
  }
  return entity;
}

/// The level: a 20 by 20 m floor (top at y = 0, a Static body), a wall
/// across x = 0 open past z = 4, a 0.3 m platform with no body, a 3 by 3 m
/// dynamic crate (wide enough that its top would survive erosion were it
/// sampled) and a trigger volume standing on the floor.
std::unique_ptr<World> make_level() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return world;
  }
  world->end_frame_phase();
  static_cast<void>(
      add_box(*world, Vec3(0.0F, -0.5F, 0.0F), Vec3(10.0F, 0.5F, 10.0F), 2));
  static_cast<void>(
      add_box(*world, Vec3(0.0F, 1.0F, -3.0F), Vec3(0.5F, 1.0F, 7.0F)));
  static_cast<void>(
      add_box(*world, Vec3(7.5F, 0.15F, -7.5F), Vec3(1.5F, 0.15F, 1.5F)));
  static_cast<void>(
      add_box(*world, Vec3(5.0F, 0.5F, 5.0F), Vec3(1.5F, 0.5F, 1.5F), 0));
  static_cast<void>(add_box(*world, Vec3(-5.0F, 1.0F, -5.0F),
                            Vec3(1.0F, 1.0F, 1.0F), -1, true));
  return world;
}

nav::NavBakeSettings settings() {
  nav::NavBakeSettings s{};
  s.boundsMin = Vec3(-10.0F, -1.0F, -10.0F);
  s.boundsMax = Vec3(10.0F, 4.0F, 10.0F);
  return s;
}

/// The surfaces of the column holding world (x, z).
std::size_t surfaces_at(const nav::NavMesh &mesh, float x, float z,
                        float *topHeight) {
  std::int32_t cx = 0;
  std::int32_t cz = 0;
  if (!mesh.column_of(x, z, &cx, &cz)) {
    return 0U;
  }
  *topHeight = mesh.surface_height(cx, cz, 0U);
  return mesh.surface_count(cx, cz);
}

} // namespace

int main() {
  std::unique_ptr<World> world = make_level();
  if (world == nullptr) {
    std::fprintf(stderr, "FAIL: could not allocate a World\n");
    return 1;
  }
  nav::NavMesh mesh{};
  g_tests.check(
      engine::runtime::bake_navigation_mesh(*world, settings(), &mesh) &&
          !mesh.empty(),
      "the level bakes through the physics queries");

  float height = -1.0F;
  g_tests.check((surfaces_at(mesh, 5.1F, 5.1F, &height) == 1U) &&
                    (std::fabs(height) < 1.0e-5F),
                "the floor under a dynamic crate is walkable: the crate is "
                "not part of the level");
  g_tests.check((surfaces_at(mesh, -5.1F, -5.1F, &height) == 1U) &&
                    (std::fabs(height) < 1.0e-5F),
                "the floor in a trigger volume is walkable");
  g_tests.check(surfaces_at(mesh, 0.1F, -3.0F, &height) == 0U,
                "the wall's footprint is not walkable");

  nav::NavQuery query{};
  Vec3 path[32] = {};
  std::size_t count = 0U;
  const bool around =
      query.init(mesh) &&
      (query.find_path(Vec3(-5.0F, 0.0F, 0.0F), Vec3(5.0F, 0.0F, 0.0F), path,
                       32U, &count) == nav::NavPathResult::Found);
  bool clearsWall = around && (count >= 3U);
  for (std::size_t i = 0U; i < count; ++i) {
    const float dx = std::fmax(std::fabs(path[i].x) - 0.5F, 0.0F);
    const float dz = std::fmax(path[i].z - 4.0F, 0.0F);
    clearsWall = clearsWall && (std::sqrt((dx * dx) + (dz * dz)) >= 0.275F);
  }
  g_tests.check(clearsWall, "a path across the wall goes around its open end");

  g_tests.check(
      (query.find_path(Vec3(-5.0F, 0.0F, -7.5F), Vec3(7.5F, 0.3F, -7.5F), path,
                       32U, &count) == nav::NavPathResult::Found) &&
          (std::fabs(path[count - 1U].y - 0.3F) < 1.0e-5F),
      "the platform within the climb is reached at its height");

  nav::NavMesh again{};
  g_tests.check(
      engine::runtime::bake_navigation_mesh(*world, settings(), &again) &&
          (again.content_hash() == mesh.content_hash()),
      "the same World bakes to the same mesh");
  return g_tests.finish("navigation bake tests");
}
