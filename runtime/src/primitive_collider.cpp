// Implements the built-in primitives' colliders: the one map from a
// HullSource to its physics builder, and the collider description every
// spawn path (script, editor) installs.

#include "engine/runtime/primitive_collider.h"

#include "engine/physics/primitive_hulls.h"
#include "engine/renderer/mesh_primitives.h"
#include "primitive_hull_build.h"

namespace engine::runtime {

bool build_primitive_hull(HullSource source,
                          physics::ConvexHullData *outHull) noexcept {
  if (outHull == nullptr) {
    return false;
  }

  switch (source) {
  case HullSource::Cylinder:
    return physics::build_cylinder_hull(outHull);
  case HullSource::Pyramid:
    return physics::build_pyramid_hull(outHull);
  case HullSource::None:
  default:
    return false;
  }
}

bool apply_primitive_hull(HullSource source, Collider *collider) noexcept {
  if (collider == nullptr) {
    return false;
  }

  physics::ConvexHullData hull{};
  if (!build_primitive_hull(source, &hull)) {
    return false;
  }

  // The payload itself is deliberately discarded: World::add_collider
  // rebuilds it from the provenance on every install path, so a caller
  // holding a second copy is exactly the drift this indirection removes.
  collider->shape = ColliderShape::ConvexHull;
  collider->hullSource = source;
  collider->halfExtents = hull.localHalfExtents;
  return true;
}

Collider primitive_collider(PrimitiveShape shape) noexcept {
  Collider collider{};
  collider.shape = ColliderShape::AABB;
  collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  switch (shape) {
  case PrimitiveShape::Sphere:
    collider.shape = ColliderShape::Sphere;
    break;
  case PrimitiveShape::Cylinder:
    collider.shape = ColliderShape::Capsule;
    static_cast<void>(apply_primitive_hull(HullSource::Cylinder, &collider));
    break;
  case PrimitiveShape::Capsule:
    collider.shape = ColliderShape::Capsule;
    break;
  case PrimitiveShape::Pyramid:
    collider.halfExtents = math::Vec3(0.5F, 0.5F, 0.58F);
    static_cast<void>(apply_primitive_hull(HullSource::Pyramid, &collider));
    break;
  case PrimitiveShape::Plane: {
    // Thin enough to read as the plane, thick enough that a resting body
    // does not tunnel it. Its centre sits half its thickness below the
    // surface, so the top is exactly the height the mesh is drawn at.
    constexpr float kHalfThickness = 0.1F;
    collider.halfExtents = math::Vec3(5.0F, kHalfThickness, 5.0F);
    collider.localPosition = math::Vec3(
        0.0F, renderer::kBuiltinPlaneSurfaceY - kHalfThickness, 0.0F);
    break;
  }
  case PrimitiveShape::Cube:
  default:
    break;
  }
  return collider;
}

} // namespace engine::runtime
