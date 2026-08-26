// Implements the built-in primitives' hull provenance: the one map from a
// HullSource to its physics builder, and the collider description every
// spawn path (script, editor) derives from it (#310).

#include "engine/runtime/primitive_collider.h"

#include "engine/physics/primitive_hulls.h"
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

} // namespace engine::runtime
