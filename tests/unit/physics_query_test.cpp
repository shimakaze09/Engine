// Verifies physics query test behavior for the Engine test suite.

#include <cmath>
#include <cstdio>
#include <memory>
#include <new>

#include "engine/math/quat.h"
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

/// Verifies ray direction magnitude cannot rescale world-space distances.
int test_raycast_direction_is_normalized() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity target = make_box(*world, math::Vec3(5.0F, 0.0F, 0.0F),
                                 math::Vec3(0.5F, 0.5F, 0.5F));
  PhysicsRaycastHit hits[1]{};
  const std::size_t count =
      physics::raycast_all(*world, math::Vec3(0.0F, 0.0F, 0.0F),
                           math::Vec3(2.0F, 0.0F, 0.0F), 10.0F, hits, 1U);
  if ((count != 1U) || (hits[0].entity != target) ||
      (hits[0].distance != 4.5F) || (hits[0].point.x != 4.5F) ||
      (hits[0].point.y != 0.0F) || (hits[0].point.z != 0.0F)) {
    return 2;
  }

  PhysicsRaycastHit closest{};
  if (!engine::runtime::raycast(*world, math::Vec3(0.0F, 0.0F, 0.0F),
                                math::Vec3(2.0F, 0.0F, 0.0F), 10.0F,
                                &closest) ||
      (closest.entity != target) || (closest.distance != 4.5F) ||
      (closest.point.x != 4.5F)) {
    return 3;
  }

  return 0;
}

/// Verifies bounded multi-hit rays retain the nearest results, not dense order.
int test_raycast_all_bounded_keeps_nearest() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  static_cast<void>(make_sphere(*world, math::Vec3(8.0F, 0.0F, 0.0F), 0.5F));
  const Entity nearEntity =
      make_sphere(*world, math::Vec3(2.0F, 0.0F, 0.0F), 0.5F);

  PhysicsRaycastHit hit{};
  const std::size_t count =
      physics::raycast_all(*world, math::Vec3(0.0F, 0.0F, 0.0F),
                           math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, &hit, 1U);
  if ((count != 1U) || (hit.entity != nearEntity) || (hit.distance != 1.5F) ||
      (hit.point.x != 1.5F)) {
    return 2;
  }

  return 0;
}

/// Verifies sweep distances use world units and zero-length ranges are safe.
int test_sweep_direction_and_zero_range() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  const Entity target = make_box(*world, math::Vec3(5.0F, 0.0F, 0.0F),
                                 math::Vec3(0.5F, 2.0F, 2.0F));
  physics::SweepHit hit{};
  if (!physics::sweep_sphere(*world, math::Vec3(0.0F, 0.0F, 0.0F), 0.5F,
                             math::Vec3(2.0F, 0.0F, 0.0F), 10.0F, &hit) ||
      (hit.entityIndex != target.index) || (hit.distance != 4.0F) ||
      (hit.timeOfImpact != 0.4F) || (hit.contactPoint.x != 4.0F)) {
    return 2;
  }

  physics::SweepHit zeroRangeHit{};
  if (physics::sweep_sphere(*world, math::Vec3(0.0F, 0.0F, 0.0F), 0.5F,
                            math::Vec3(1.0F, 0.0F, 0.0F), 0.0F,
                            &zeroRangeHit)) {
    return 3;
  }
  if (physics::sweep_box(*world, math::Vec3(0.0F, 0.0F, 0.0F),
                         math::Vec3(0.5F, 0.5F, 0.5F),
                         math::Vec3(1.0F, 0.0F, 0.0F), 0.0F, &zeroRangeHit)) {
    return 4;
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

/// Verifies queries use parent TRS plus collider-local position and rotation.
int test_parented_trs_collider_queries() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();

  constexpr float kQuarterTurnSinCos = 0.7071067811865475F;
  const math::Quat quarterTurn(0.0F, 0.0F, kQuarterTurnSinCos,
                               kQuarterTurnSinCos);
  const Entity parent = world->create_entity();
  Transform parentTransform{};
  parentTransform.position = math::Vec3(10.0F, 0.0F, 0.0F);
  parentTransform.rotation = quarterTurn;
  parentTransform.scale = math::Vec3(2.0F, 3.0F, 1.0F);
  if (!world->add_transform(parent, parentTransform)) {
    return 2;
  }

  const Entity child = world->create_entity();
  Transform childTransform{};
  childTransform.position = math::Vec3(1.0F, 0.0F, 0.0F);
  childTransform.scale = math::Vec3(0.5F, 2.0F, 1.0F);
  childTransform.parentId = world->persistent_id(parent);
  if (!world->add_transform(child, childTransform)) {
    return 3;
  }

  Collider collider{};
  collider.halfExtents = math::Vec3(1.0F, 0.5F, 0.5F);
  collider.localPosition = math::Vec3(1.0F, 0.0F, 0.0F);
  collider.localRotation = quarterTurn;
  if (!world->add_collider(child, collider)) {
    return 4;
  }

  PhysicsRaycastHit rayHit{};
  const std::size_t rayCount =
      physics::raycast_all(*world, math::Vec3(0.0F, 3.0F, 0.0F),
                           math::Vec3(4.0F, 0.0F, 0.0F), 20.0F, &rayHit, 1U);
  if ((rayCount != 1U) || (rayHit.entity != child) ||
      (std::fabs(rayHit.distance - 4.0F) > 1.0e-4F) ||
      (std::fabs(rayHit.point.x - 4.0F) > 1.0e-4F) ||
      (std::fabs(rayHit.normal.x + 1.0F) > 1.0e-4F)) {
    return 5;
  }

  std::uint32_t overlaps[1]{};
  const std::size_t overlapCount = physics::overlap_sphere(
      *world, math::Vec3(10.0F, 3.0F, 0.0F), 0.1F, overlaps, 1U);
  if ((overlapCount != 1U) || (overlaps[0] != child.index)) {
    return 6;
  }

  physics::SweepHit sweepHit{};
  if (!physics::sweep_sphere(*world, math::Vec3(-5.0F, 3.0F, 0.0F), 1.0F,
                             math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, &sweepHit) ||
      (sweepHit.entityIndex != child.index) ||
      (std::fabs(sweepHit.distance - 8.0F) > 1.0e-4F) ||
      (std::fabs(sweepHit.timeOfImpact - 0.4F) > 1.0e-5F)) {
    return 7;
  }

  const Entity fitParent = world->create_entity();
  Transform fitParentTransform{};
  fitParentTransform.position = math::Vec3(-10.0F, 0.0F, 0.0F);
  fitParentTransform.rotation =
      math::Quat(0.0F, 0.0F, 0.3826834323650898F, 0.9238795325112867F);
  world->add_transform(fitParent, fitParentTransform);
  const Entity fitChild = world->create_entity();
  Transform fitChildTransform{};
  fitChildTransform.parentId = world->persistent_id(fitParent);
  world->add_transform(fitChild, fitChildTransform);
  Collider fitCollider{};
  fitCollider.halfExtents = math::Vec3(2.0F, 0.1F, 0.5F);
  world->add_collider(fitChild, fitCollider);
  std::uint32_t falsePositive[1]{};
  if (physics::overlap_sphere(*world, math::Vec3(-8.7F, -1.3F, 0.0F), 0.05F,
                              falsePositive, 1U) != 0U) {
    return 8;
  }

  return 0;
}

// H-08 regression: sweeps must test the target's real shape, not its
// AABB. A 0.1-radius sphere swept along +X at lateral offset
// (y,z)=(0.85,0.85) misses a unit sphere at the origin (lateral distance
// 1.202 > combined radius 1.1) but passes inside the target's expanded
// AABB corner, which the old bounds-only sweep reported as a hit. The
// same sweep at (0.7,0) must still hit: entry at x = -sqrt(1.1^2-0.7^2)
// = -0.8485, so distance = 5 - 0.8485 = 4.1515 from x=-5; tolerance 1e-3
// covers the conservative-advancement step bound (~2e-5) with margin.
int test_sweep_sphere_shape_accurate() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();
  make_sphere(*world, math::Vec3(0.0F, 0.0F, 0.0F), 1.0F);

  physics::SweepHit hit{};
  if (physics::sweep_sphere(*world, math::Vec3(-5.0F, 0.85F, 0.85F), 0.1F,
                            math::Vec3(1.0F, 0.0F, 0.0F), 10.0F, &hit)) {
    std::printf("FAIL sweep_sphere_shape_accurate: corner false positive\n");
    return 2;
  }

  if (!physics::sweep_sphere(*world, math::Vec3(-5.0F, 0.7F, 0.0F), 0.1F,
                             math::Vec3(1.0F, 0.0F, 0.0F), 10.0F, &hit)) {
    std::printf("FAIL sweep_sphere_shape_accurate: real hit missed\n");
    return 3;
  }
  if (std::fabs(hit.distance - 4.1515F) > 1.0e-3F) {
    std::printf("FAIL sweep_sphere_shape_accurate: dist=%.4f\n",
                static_cast<double>(hit.distance));
    return 4;
  }
  return 0;
}

// H-08 regression: sweep_box against a sphere target must miss when only
// the target's AABB corner lies in the path. A 0.1-half-extent box swept
// along +X at (y,z)=(1.05,1.05) clears a unit sphere (nearest sphere
// point at lateral distance sqrt(2)*1.05-1 = 0.485 > 0.1*sqrt(2)) but
// intersects the AABB-expanded corner the old sweep tested. The centered
// sweep must still hit at x = -(1+0.1) => distance 5-1.1 = 3.9 (face
// contact; tolerance 1e-3 as above).
int test_sweep_box_shape_accurate() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }
  world->end_frame_phase();
  make_sphere(*world, math::Vec3(0.0F, 0.0F, 0.0F), 1.0F);

  physics::SweepHit hit{};
  if (physics::sweep_box(*world, math::Vec3(-5.0F, 1.05F, 1.05F),
                         math::Vec3(0.1F, 0.1F, 0.1F),
                         math::Vec3(1.0F, 0.0F, 0.0F), 10.0F, &hit)) {
    std::printf("FAIL sweep_box_shape_accurate: corner false positive\n");
    return 2;
  }

  if (!physics::sweep_box(*world, math::Vec3(-5.0F, 0.0F, 0.0F),
                          math::Vec3(0.1F, 0.1F, 0.1F),
                          math::Vec3(1.0F, 0.0F, 0.0F), 10.0F, &hit)) {
    std::printf("FAIL sweep_box_shape_accurate: real hit missed\n");
    return 3;
  }
  if (std::fabs(hit.distance - 3.9F) > 1.0e-3F) {
    std::printf("FAIL sweep_box_shape_accurate: dist=%.4f\n",
                static_cast<double>(hit.distance));
    return 4;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
// Sweep skip: skipEntity excludes the entity's own collider plus compound
// colliders it owns via rigid_body_owner, while other colliders still hit.
int test_sweep_sphere_skip_entity() noexcept {
  std::unique_ptr<World> world(new (std::nothrow) World());
  if (world == nullptr) {
    return 1;
  }

  Transform selfLocal{};
  const Entity self = world->create_scene_object(selfLocal);
  Collider selfCol{};
  selfCol.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(self, selfCol)) {
    return 2;
  }
  engine::runtime::RigidBody body{};
  body.inverseMass = 1.0F;
  if (!world->add_rigid_body(self, body)) {
    return 3;
  }

  Transform childLocal{};
  childLocal.position = math::Vec3(2.0F, 0.0F, 0.0F);
  childLocal.parentId = world->persistent_id(self);
  const Entity child = world->create_scene_object(childLocal);
  Collider childCol{};
  childCol.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  if (!world->add_collider(child, childCol)) {
    return 4;
  }

  make_box(*world, math::Vec3(6.0F, 0.0F, 0.0F),
           math::Vec3(0.5F, 2.0F, 2.0F));
  world->begin_transform_phase();
  world->end_frame_phase();

  physics::SweepHit hit{};
  if (!physics::sweep_sphere(*world, math::Vec3(-3.0F, 0.0F, 0.0F), 0.5F,
                             math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, &hit,
                             0xFFFFFFFFU, self)) {
    std::printf("FAIL sweep_sphere_skip_entity: no hit past compound\n");
    return 5;
  }
  if (std::fabs(hit.distance - 8.0F) > 0.0001F) {
    std::printf("FAIL sweep_sphere_skip_entity: dist=%.3f (expected 8.0)\n",
                hit.distance);
    return 6;
  }

  physics::SweepHit unskipped{};
  if (!physics::sweep_sphere(*world, math::Vec3(-3.0F, 0.0F, 0.0F), 0.5F,
                             math::Vec3(1.0F, 0.0F, 0.0F), 20.0F, &unskipped) ||
      (unskipped.entityIndex != self.index)) {
    std::printf("FAIL sweep_sphere_skip_entity: default sweep missed self\n");
    return 7;
  }
  return 0;
}

int main() {
  struct TestCase {
    const char *name;
    int (*func)();
  };

  const TestCase tests[] = {
      {"raycast_all_3_spheres", test_raycast_all_3_spheres},
      {"raycast_all_mask", test_raycast_all_mask},
      {"raycast_direction_is_normalized", test_raycast_direction_is_normalized},
      {"raycast_all_bounded_keeps_nearest",
       test_raycast_all_bounded_keeps_nearest},
      {"sweep_direction_and_zero_range", test_sweep_direction_and_zero_range},
      {"overlap_sphere_cluster", test_overlap_sphere_cluster},
      {"overlap_box", test_overlap_box},
      {"overlap_sphere_mask", test_overlap_sphere_mask},
      {"sweep_sphere_wall", test_sweep_sphere_wall},
      {"sweep_box_wall", test_sweep_box_wall},
      {"sweep_sphere_shape_accurate", test_sweep_sphere_shape_accurate},
      {"sweep_box_shape_accurate", test_sweep_box_shape_accurate},
      {"parented_trs_collider_queries", test_parented_trs_collider_queries},
      {"sweep_sphere_skip_entity", test_sweep_sphere_skip_entity},
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
