// Verifies convex-hull provenance behavior: World::add_collider rebuilds the
// canonical primitive hull recorded in Collider::hullSource, the collider
// description spawn paths derive from that provenance (#310) matches the
// canonical builder, the scene serializer round-trips the field, and buffer
// save/load (the editor's play snapshot/Stop-restore mechanism) restores hull
// payloads exactly.

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

#include "engine/physics/physics.h"
#include "engine/physics/primitive_hulls.h"
#include "engine/runtime/physics_bridge.h"
#include "engine/runtime/primitive_collider.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

namespace {

constexpr std::size_t kSceneBufferCapacity = 65536U;

/// Compares every meaningful hull field with exact equality (no tolerances):
/// counts, all used planes and vertices, and the cached local AABB.
bool hulls_exactly_equal(const engine::physics::ConvexHullData &lhs,
                         const engine::physics::ConvexHullData &rhs) noexcept {
  if ((lhs.planeCount != rhs.planeCount) ||
      (lhs.vertexCount != rhs.vertexCount)) {
    return false;
  }
  for (std::size_t i = 0U; i < lhs.planeCount; ++i) {
    if ((lhs.planes[i].normal.x != rhs.planes[i].normal.x) ||
        (lhs.planes[i].normal.y != rhs.planes[i].normal.y) ||
        (lhs.planes[i].normal.z != rhs.planes[i].normal.z) ||
        (lhs.planes[i].distance != rhs.planes[i].distance)) {
      return false;
    }
  }
  for (std::size_t i = 0U; i < lhs.vertexCount; ++i) {
    if ((lhs.vertices[i].x != rhs.vertices[i].x) ||
        (lhs.vertices[i].y != rhs.vertices[i].y) ||
        (lhs.vertices[i].z != rhs.vertices[i].z)) {
      return false;
    }
  }
  return (lhs.localCenter.x == rhs.localCenter.x) &&
         (lhs.localCenter.y == rhs.localCenter.y) &&
         (lhs.localCenter.z == rhs.localCenter.z) &&
         (lhs.localHalfExtents.x == rhs.localHalfExtents.x) &&
         (lhs.localHalfExtents.y == rhs.localHalfExtents.y) &&
         (lhs.localHalfExtents.z == rhs.localHalfExtents.z);
}

/// Creates an entity with a transform and the given collider; returns the
/// entity or kInvalidEntity on any failure.
engine::runtime::Entity
add_collider_entity(engine::runtime::World &world,
                    const engine::runtime::Collider &collider) noexcept {
  const engine::runtime::Entity entity = world.create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  engine::runtime::Transform transform{};
  if (!world.add_transform(entity, transform) ||
      !world.add_collider(entity, collider)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

/// Builds the collider used by every hull test for one provenance value.
engine::runtime::Collider
make_hull_collider(engine::runtime::HullSource source,
                   const engine::math::Vec3 &halfExtents) noexcept {
  engine::runtime::Collider collider{};
  collider.shape = engine::runtime::ColliderShape::ConvexHull;
  collider.hullSource = source;
  collider.halfExtents = halfExtents;
  return collider;
}

/// Verifies add_collider installs the canonical hull payload for each
/// provenance value and installs nothing when provenance is None.
int verify_add_collider_installs_provenance_hull() {
  engine::physics::ConvexHullData cylinderHull{};
  engine::physics::ConvexHullData pyramidHull{};
  if (!engine::physics::build_cylinder_hull(&cylinderHull) ||
      !engine::physics::build_pyramid_hull(&pyramidHull)) {
    return 10;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 11;
  }

  const engine::runtime::Entity cylinder = add_collider_entity(
      *world, make_hull_collider(engine::runtime::HullSource::Cylinder,
                                 cylinderHull.localHalfExtents));
  const engine::runtime::Entity pyramid = add_collider_entity(
      *world, make_hull_collider(engine::runtime::HullSource::Pyramid,
                                 pyramidHull.localHalfExtents));
  const engine::runtime::Entity bare = add_collider_entity(
      *world, make_hull_collider(engine::runtime::HullSource::None,
                                 engine::math::Vec3(0.5F, 0.5F, 0.5F)));
  if ((cylinder == engine::runtime::kInvalidEntity) ||
      (pyramid == engine::runtime::kInvalidEntity) ||
      (bare == engine::runtime::kInvalidEntity)) {
    return 12;
  }

  const engine::physics::ConvexHullData *installedCylinder =
      engine::runtime::get_convex_hull_data(*world, cylinder);
  if ((installedCylinder == nullptr) ||
      !hulls_exactly_equal(*installedCylinder, cylinderHull)) {
    return 13;
  }

  const engine::physics::ConvexHullData *installedPyramid =
      engine::runtime::get_convex_hull_data(*world, pyramid);
  if ((installedPyramid == nullptr) ||
      !hulls_exactly_equal(*installedPyramid, pyramidHull)) {
    return 14;
  }

  if (engine::runtime::get_convex_hull_data(*world, bare) != nullptr) {
    return 15;
  }

  return 0;
}

/// Verifies the collider description spawn paths derive from provenance
/// (issue #310): apply_primitive_hull reproduces the canonical builder's own
/// half extents exactly, leaves a hull-less source untouched so the caller's
/// authored fallback stands, and describes a collider whose installed payload
/// matches the builder. Also pins has_convex_hull_payload, the observable a
/// spawn uses to see whether the rebuild found a free hull slot.
int verify_apply_primitive_hull_describes_canonical_collider() {
  engine::physics::ConvexHullData cylinderHull{};
  engine::physics::ConvexHullData pyramidHull{};
  if (!engine::physics::build_cylinder_hull(&cylinderHull) ||
      !engine::physics::build_pyramid_hull(&pyramidHull)) {
    return 70;
  }

  // The authored fallback a caller brings: what must survive when the source
  // names no hull, and what must be replaced when it does.
  engine::runtime::Collider cylinder{};
  cylinder.shape = engine::runtime::ColliderShape::Capsule;
  cylinder.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.5F);
  if (!engine::runtime::apply_primitive_hull(
          engine::runtime::HullSource::Cylinder, &cylinder)) {
    return 71;
  }
  if ((cylinder.shape != engine::runtime::ColliderShape::ConvexHull) ||
      (cylinder.hullSource != engine::runtime::HullSource::Cylinder) ||
      (cylinder.halfExtents.x != cylinderHull.localHalfExtents.x) ||
      (cylinder.halfExtents.y != cylinderHull.localHalfExtents.y) ||
      (cylinder.halfExtents.z != cylinderHull.localHalfExtents.z)) {
    return 72;
  }

  engine::runtime::Collider pyramid{};
  pyramid.shape = engine::runtime::ColliderShape::AABB;
  pyramid.halfExtents = engine::math::Vec3(0.5F, 0.5F, 0.58F);
  if (!engine::runtime::apply_primitive_hull(
          engine::runtime::HullSource::Pyramid, &pyramid)) {
    return 73;
  }
  if ((pyramid.shape != engine::runtime::ColliderShape::ConvexHull) ||
      (pyramid.hullSource != engine::runtime::HullSource::Pyramid) ||
      (pyramid.halfExtents.x != pyramidHull.localHalfExtents.x) ||
      (pyramid.halfExtents.y != pyramidHull.localHalfExtents.y) ||
      (pyramid.halfExtents.z != pyramidHull.localHalfExtents.z)) {
    return 74;
  }

  engine::runtime::Collider untouched{};
  untouched.shape = engine::runtime::ColliderShape::Capsule;
  untouched.halfExtents = engine::math::Vec3(1.0F, 2.0F, 3.0F);
  if (engine::runtime::apply_primitive_hull(engine::runtime::HullSource::None,
                                            &untouched)) {
    return 75;
  }
  if ((untouched.shape != engine::runtime::ColliderShape::Capsule) ||
      (untouched.hullSource != engine::runtime::HullSource::None) ||
      (untouched.halfExtents.x != 1.0F) || (untouched.halfExtents.y != 2.0F) ||
      (untouched.halfExtents.z != 3.0F)) {
    return 76;
  }

  if (engine::runtime::apply_primitive_hull(
          engine::runtime::HullSource::Cylinder, nullptr)) {
    return 77;
  }

  // Production parity: the described collider installs the builder's payload.
  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 78;
  }
  const engine::runtime::Entity described = add_collider_entity(*world,
                                                                cylinder);
  const engine::runtime::Entity bare = add_collider_entity(
      *world, make_hull_collider(engine::runtime::HullSource::None,
                                 engine::math::Vec3(0.5F, 0.5F, 0.5F)));
  if ((described == engine::runtime::kInvalidEntity) ||
      (bare == engine::runtime::kInvalidEntity)) {
    return 79;
  }
  const engine::physics::ConvexHullData *installed =
      engine::runtime::get_convex_hull_data(*world, described);
  if ((installed == nullptr) || !hulls_exactly_equal(*installed,
                                                     cylinderHull)) {
    return 80;
  }
  if (!world->has_convex_hull_payload(described) ||
      world->has_convex_hull_payload(bare)) {
    return 81;
  }

  return 0;
}

/// Verifies a saved scene restores hullSource and the exact hull payload on
/// load, and that a ConvexHull collider without provenance stays payload-free.
int verify_hull_scene_round_trip() {
  engine::physics::ConvexHullData cylinderHull{};
  engine::physics::ConvexHullData pyramidHull{};
  if (!engine::physics::build_cylinder_hull(&cylinderHull) ||
      !engine::physics::build_pyramid_hull(&pyramidHull)) {
    return 20;
  }

  std::unique_ptr<engine::runtime::World> source(new (std::nothrow)
                                                     engine::runtime::World());
  if (source == nullptr) {
    return 21;
  }
  if ((add_collider_entity(
           *source, make_hull_collider(engine::runtime::HullSource::Cylinder,
                                       cylinderHull.localHalfExtents)) ==
       engine::runtime::kInvalidEntity) ||
      (add_collider_entity(
           *source, make_hull_collider(engine::runtime::HullSource::Pyramid,
                                       pyramidHull.localHalfExtents)) ==
       engine::runtime::kInvalidEntity) ||
      (add_collider_entity(
           *source, make_hull_collider(engine::runtime::HullSource::None,
                                       engine::math::Vec3(9.0F, 9.0F, 9.0F))) ==
       engine::runtime::kInvalidEntity)) {
    return 22;
  }

  std::unique_ptr<char[]> buffer(new (std::nothrow) char[kSceneBufferCapacity]);
  if (buffer == nullptr) {
    return 23;
  }
  std::size_t sceneSize = 0U;
  if (!engine::runtime::save_scene(*source, buffer.get(), kSceneBufferCapacity,
                                   &sceneSize)) {
    return 24;
  }

  std::unique_ptr<engine::runtime::World> target(new (std::nothrow)
                                                     engine::runtime::World());
  if (target == nullptr) {
    return 25;
  }
  if (!engine::runtime::load_scene(*target, buffer.get(), sceneSize)) {
    return 26;
  }

  int result = 0;
  std::size_t colliderCount = 0U;
  target->for_each_alive([&](engine::runtime::Entity entity) {
    engine::runtime::Collider collider{};
    if (!target->get_collider(entity, &collider)) {
      return;
    }
    ++colliderCount;

    const engine::physics::ConvexHullData *payload =
        engine::runtime::get_convex_hull_data(*target, entity);
    switch (collider.hullSource) {
    case engine::runtime::HullSource::Cylinder:
      if ((payload == nullptr) ||
          !hulls_exactly_equal(*payload, cylinderHull)) {
        result = 27;
      }
      break;
    case engine::runtime::HullSource::Pyramid:
      if ((payload == nullptr) || !hulls_exactly_equal(*payload, pyramidHull)) {
        result = 28;
      }
      break;
    case engine::runtime::HullSource::None:
      if ((payload != nullptr) || (collider.halfExtents.x != 9.0F)) {
        result = 29;
      }
      break;
    }
  });
  if (result != 0) {
    return result;
  }
  if (colliderCount != 3U) {
    return 30;
  }

  return 0;
}

/// Verifies save → load → save produces byte-identical scene JSON when hull
/// colliders are present (serialization determinism).
int verify_hull_scene_save_load_save_byte_identical() {
  engine::physics::ConvexHullData cylinderHull{};
  if (!engine::physics::build_cylinder_hull(&cylinderHull)) {
    return 40;
  }

  std::unique_ptr<engine::runtime::World> source(new (std::nothrow)
                                                     engine::runtime::World());
  if (source == nullptr) {
    return 41;
  }
  if (add_collider_entity(
          *source, make_hull_collider(engine::runtime::HullSource::Cylinder,
                                      cylinderHull.localHalfExtents)) ==
      engine::runtime::kInvalidEntity) {
    return 42;
  }

  std::unique_ptr<char[]> firstBuffer(new (std::nothrow)
                                          char[kSceneBufferCapacity]);
  std::unique_ptr<char[]> secondBuffer(new (std::nothrow)
                                           char[kSceneBufferCapacity]);
  if ((firstBuffer == nullptr) || (secondBuffer == nullptr)) {
    return 43;
  }

  std::size_t firstSize = 0U;
  if (!engine::runtime::save_scene(*source, firstBuffer.get(),
                                   kSceneBufferCapacity, &firstSize)) {
    return 44;
  }

  std::unique_ptr<engine::runtime::World> reloaded(new (std::nothrow)
                                                       engine::runtime::World());
  if (reloaded == nullptr) {
    return 45;
  }
  if (!engine::runtime::load_scene(*reloaded, firstBuffer.get(), firstSize)) {
    return 46;
  }

  std::size_t secondSize = 0U;
  if (!engine::runtime::save_scene(*reloaded, secondBuffer.get(),
                                   kSceneBufferCapacity, &secondSize)) {
    return 47;
  }

  if ((firstSize != secondSize) ||
      (std::memcmp(firstBuffer.get(), secondBuffer.get(), firstSize) != 0)) {
    return 48;
  }

  return 0;
}

/// Verifies the editor Stop-restore mechanism: a buffer snapshot taken before
/// destructive edits restores the hull payload and provenance exactly.
int verify_play_snapshot_restore_preserves_hull() {
  engine::physics::ConvexHullData cylinderHull{};
  if (!engine::physics::build_cylinder_hull(&cylinderHull)) {
    return 60;
  }

  std::unique_ptr<engine::runtime::World> world(new (std::nothrow)
                                                    engine::runtime::World());
  if (world == nullptr) {
    return 61;
  }
  const engine::runtime::Entity entity = add_collider_entity(
      *world, make_hull_collider(engine::runtime::HullSource::Cylinder,
                                 cylinderHull.localHalfExtents));
  if (entity == engine::runtime::kInvalidEntity) {
    return 62;
  }

  std::unique_ptr<char[]> snapshot(new (std::nothrow)
                                       char[kSceneBufferCapacity]);
  if (snapshot == nullptr) {
    return 63;
  }
  std::size_t snapshotSize = 0U;
  if (!engine::runtime::save_scene(*world, snapshot.get(),
                                   kSceneBufferCapacity, &snapshotSize)) {
    return 64;
  }

  world->destroy_entity(entity);
  if (engine::runtime::get_convex_hull_data(*world, entity) != nullptr) {
    return 65;
  }

  if (!engine::runtime::load_scene(*world, snapshot.get(), snapshotSize)) {
    return 66;
  }

  int result = 67;
  world->for_each_alive([&](engine::runtime::Entity restored) {
    engine::runtime::Collider collider{};
    if (!world->get_collider(restored, &collider) ||
        (collider.hullSource != engine::runtime::HullSource::Cylinder)) {
      return;
    }
    const engine::physics::ConvexHullData *payload =
        engine::runtime::get_convex_hull_data(*world, restored);
    if ((payload != nullptr) && hulls_exactly_equal(*payload, cylinderHull)) {
      result = 0;
    }
  });

  return result;
}

} // namespace

/// Runs every hull provenance suite in order and returns the first failure.
int main() {
  int result = verify_add_collider_installs_provenance_hull();
  if (result != 0) {
    return result;
  }

  result = verify_apply_primitive_hull_describes_canonical_collider();
  if (result != 0) {
    return result;
  }

  result = verify_hull_scene_round_trip();
  if (result != 0) {
    return result;
  }

  result = verify_hull_scene_save_load_save_byte_identical();
  if (result != 0) {
    return result;
  }

  return verify_play_snapshot_restore_preserves_hull();
}
