// Declares reference-face contact clipping: multi-point manifolds for faceted
// colliders so resting boxes get face support and overhanging boxes can tip.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/collider.h"

#include <cstddef>

namespace engine::physics {

/// Up to four world-space contact points sharing one contact normal.
struct ClippedManifold final {
  static constexpr std::size_t kMaxPoints = 4U;
  math::Vec3 points[kMaxPoints]{};
  float penetrations[kMaxPoints]{};
  std::size_t count = 0U;
};

/// Clips the pair's touching faces along the A→B contact normal into a
/// manifold; false when either shape is not faceted (sphere/capsule/
/// heightfield) or clipping degenerates, so callers fall back to the
/// single-point contact.
bool clip_contact_manifold(const ColliderWorldGeometry &geometryA,
                           const ColliderWorldGeometry &geometryB,
                           const math::Vec3 &normal,
                           ClippedManifold *outManifold) noexcept;

} // namespace engine::physics
