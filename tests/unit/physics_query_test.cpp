#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/vec3.h"
#include "engine/physics/physics_query.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/world.h"

namespace {

using World = engine::runtime::World;
using Entity = engine::runtime::Entity;
using Transform = engine::runtime::Transform;
using Collider = engine::runtime::Collider;
using PhysicsRaycastHit = engine::runtime::PhysicsRaycastHit;
namespace math = engine::math;
namespace physics = engine::physics;

// Helper: create a static sphere collider at a position.
Entity make_sphere(World &w, const math::Vec3 &pos, float radius,
                   std::uint32_t layer = 1U) noexcept {
  const Entity e = w.create_entity();
  Transform t{};
  t.position = pos;
  w.add_transform(e, t);
  Collider col{};
  col.shape = engine::runtime::ColliderShape::Sphere;
  col.halfExtents = math::Vec3(radius, radius, radius);
  col.collisionLayer = layer;
  col.collisionMask = 0xFFFFFFFFU;
  w.add_collider(e, col);
  return e;
}

// Helper: create a static box collider at a position.
Entity make_box(World &w, const math::Vec3 &pos, const math::Vec3 &halfExtents,
                std::uint32_t layer = 1U) noexcept {
  const Entity e = w.create_entity();
  Transform t{};
  t.position = pos;
  w.add_transform(e, t);
  Collider col{};
  col.halfExtents = halfExtents;
  col.collisionLayer = layer;
  col.collisionMask = 0xFFFFFFFFU;
  w.add_collider(e, col);
  return e;
}

// D1f: Ray through 3 aligned spheres returns 3 hits sorted by distance.
int test_raycast_all_3_spheres() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  // Place 3 spheres along the X axis at x=2, x=5, x=8.
  make_sphere(*world, math::Vec3(2.0F, 0.0F, 0.0F), 0.5F);
  make_sphere(*world, math::Vec3(5.0F, 0.0F, 0.0F), 0.5F);
  make_sphere(*world, math::Vec3(8.0F, 0.0F, 0.0F), 0.5F);

  PhysicsRaycastHit hits[8]{};
  const std::size_t count =
      physics::raycast_all(*world, math::Vec3(0.0F, 0.0F, 0.0F),
                           math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, hits, 8U);

  if (count != 3U) {
    std::printf("FAIL raycast_all_3_spheres: count=%zu (expected 3)\n", count);
    return 2;
  }

  // Verify sorted by distance: hit[0].distance < hit[1] < hit[2].
  for (std::size_t i = 1U; i < count; ++i) {
    if (hits[i].distance < hits[i - 1U].distance) {
      std::printf("FAIL raycast_all_3_spheres: not sorted at %zu\n", i);
      return 3;
    }
  }

  // First hit should be near x=1.5 (sphere at x=2 with radius 0.5).
  if (std::fabs(hits[0U].distance - 1.5F) > 0.2F) {
    std::printf("FAIL raycast_all_3_spheres: first hit dist=%.3f\n",
                hits[0U].distance);
    return 4;
  }

  return 0;
}

// D1f supplementary: raycast with mask filters correctly.
int test_raycast_all_mask() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  make_sphere(*world, math::Vec3(2.0F, 0.0F, 0.0F), 0.5F, 1U);
  make_sphere(*world, math::Vec3(5.0F, 0.0F, 0.0F), 0.5F, 2U);
  make_sphere(*world, math::Vec3(8.0F, 0.0F, 0.0F), 0.5F, 1U);

  PhysicsRaycastHit hits[8]{};
  // Mask=1U should only hit layer 1 entities.
  const std::size_t count =
      physics::raycast_all(*world, math::Vec3(0.0F, 0.0F, 0.0F),
                           math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, hits, 8U, 1U);

  if (count != 2U) {
    std::printf("FAIL raycast_all_mask: count=%zu (expected 2)\n", count);
    return 2;
  }
  return 0;
}

// D2d: 10 entities in cluster, overlap sphere catches correct subset.
int test_overlap_sphere_cluster() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  // Place 10 spheres along X axis at x=0..9, radius 0.3.
  for (int i = 0; i < 10; ++i) {
    make_sphere(*world, math::Vec3(static_cast<float>(i), 0.0F, 0.0F), 0.3F);
  }

  // Overlap sphere at x=2, radius=1.5 should catch entities at x=1,2,3.
  std::uint32_t indices[16]{};
  const std::size_t count = physics::overlap_sphere(
      *world, math::Vec3(2.0F, 0.0F, 0.0F), 1.5F, indices, 16U);

  if (count < 2U || count > 5U) {
    std::printf("FAIL overlap_sphere_cluster: count=%zu (expected 2-5)\n",
                count);
    return 2;
  }
  return 0;
}

// D2d supplementary: overlap box test.
int test_overlap_box() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  // Place 5 boxes along X axis at x=0,2,4,6,8.
  for (int i = 0; i < 5; ++i) {
    make_box(*world, math::Vec3(static_cast<float>(i * 2), 0.0F, 0.0F),
             math::Vec3(0.5F, 0.5F, 0.5F));
  }

  // Overlap box centered at x=3, half=2.0 should catch boxes at x=2,4.
  std::uint32_t indices[16]{};
  const std::size_t count =
      physics::overlap_box(*world, math::Vec3(3.0F, 0.0F, 0.0F),
                           math::Vec3(2.0F, 1.0F, 1.0F), indices, 16U);

  if (count < 2U) {
    std::printf("FAIL overlap_box: count=%zu (expected >= 2)\n", count);
    return 2;
  }
  return 0;
}

// D2d supplementary: overlap sphere with mask.
int test_overlap_sphere_mask() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  make_sphere(*world, math::Vec3(0.0F, 0.0F, 0.0F), 1.0F, 1U);
  make_sphere(*world, math::Vec3(0.5F, 0.0F, 0.0F), 1.0F, 2U);
  make_sphere(*world, math::Vec3(1.0F, 0.0F, 0.0F), 1.0F, 4U);

  std::uint32_t indices[16]{};
  // Mask=2U should only catch layer 2 entity.
  const std::size_t count = physics::overlap_sphere(
      *world, math::Vec3(0.5F, 0.0F, 0.0F), 5.0F, indices, 16U, 2U);

  if (count != 1U) {
    std::printf("FAIL overlap_sphere_mask: count=%zu (expected 1)\n", count);
    return 2;
  }
  return 0;
}

// D3e: Sweep sphere through corridor, hits wall at correct distance.
int test_sweep_sphere_wall() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  // Place a wall (box) at x=5.
  make_box(*world, math::Vec3(5.0F, 0.0F, 0.0F), math::Vec3(0.5F, 2.0F, 2.0F));

  // Sweep a sphere of radius 0.5 from origin along +X.
  physics::SweepHit hit{};
  const bool found =
      physics::sweep_sphere(*world, math::Vec3(0.0F, 0.0F, 0.0F), 0.5F,
                            math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, &hit);

  if (!found) {
    std::printf("FAIL sweep_sphere_wall: no hit\n");
    return 2;
  }

  // Should hit around x=4.0 (wall starts at x=4.5, sphere radius 0.5).
  if (hit.distance < 2.0F || hit.distance > 5.5F) {
    std::printf("FAIL sweep_sphere_wall: dist=%.3f (expected ~4.0)\n",
                hit.distance);
    return 3;
  }
  return 0;
}

// D3e supplementary: Sweep box test.
int test_sweep_box_wall() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  // Place a wall (box) at x=10.
  make_box(*world, math::Vec3(10.0F, 0.0F, 0.0F), math::Vec3(0.5F, 2.0F, 2.0F));

  // Sweep a box of half-extents 0.5 from origin along +X.
  physics::SweepHit hit{};
  const bool found = physics::sweep_box(
      *world, math::Vec3(0.0F, 0.0F, 0.0F), math::Vec3(0.5F, 0.5F, 0.5F),
      math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, &hit);

  if (!found) {
    std::printf("FAIL sweep_box_wall: no hit\n");
    return 2;
  }

  // Should hit around x=9.0 (wall at x=9.5, box half=0.5).
  if (hit.distance < 7.0F || hit.distance > 10.5F) {
    std::printf("FAIL sweep_box_wall: dist=%.3f (expected ~9.0)\n",
                hit.distance);
    return 3;
  }
  return 0;
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    int (*func)();
  };

  const TestCase tests[] = {
      {"raycast_all_3_spheres", test_raycast_all_3_spheres},
      {"raycast_all_mask", test_raycast_all_mask},
      {"overlap_sphere_cluster", test_overlap_sphere_cluster},
      {"overlap_box", test_overlap_box},
      {"overlap_sphere_mask", test_overlap_sphere_mask},
      {"sweep_sphere_wall", test_sweep_sphere_wall},
      {"sweep_box_wall", test_sweep_box_wall},
  };

  int failures = 0;
  for (const auto &tc : tests) {
    const int result = tc.func();
    if (result != 0) {
      std::printf("FAIL %s (code %d)\n", tc.name, result);
      ++failures;
    } else {
      std::printf("PASS %s\n", tc.name);
    }
  }

  if (failures > 0) {
    std::printf("%d test(s) failed\n", failures);
    return 1;
  }
  std::printf("All physics query tests passed\n");
  return 0;
}
