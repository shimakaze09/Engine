// Ray type and slab-based ray/AABB intersection, defined inline for raycast
// and broadphase hot paths.

#pragma once

#include <cmath>
#include <limits>

#include "engine/math/aabb.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// Ray from origin along (not necessarily unit) direction.
struct Ray final {
  Vec3 origin = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 direction = Vec3(0.0F, 0.0F, 1.0F);
};

/// Component of a vector selected by axis index (0 = x, 1 = y, 2 = z);
/// distinct data members are not an array, so indexing goes through this
/// instead of pointer arithmetic on a member address.
constexpr float vec3_component(const Vec3 &value, int axis) noexcept {
  return (axis == 0) ? value.x : ((axis == 1) ? value.y : value.z);
}

// Returns true if the ray hits the box. Sets *outT to the parametric hit t
// (>= 0) along ray.direction — origin + t * direction is the hit point; the
// physical distance is t * length(direction) and equals t only for unit
// directions. The parameter is affine-invariant, which is what lets physics
// intersect in collider-local space (a non-unit direction after an inverse
// world transform) and reuse t on the world ray.
inline bool ray_intersects_aabb(const Ray &ray, const AABB &box,
                                float *outT) noexcept {
  constexpr float kInfinity = std::numeric_limits<float>::infinity();

  float tMin = 0.0F;
  float tMax = kInfinity;

  for (int i = 0; i < 3; ++i) {
    const float originAxis = vec3_component(ray.origin, i);
    const float directionAxis = vec3_component(ray.direction, i);
    const float minAxis = vec3_component(box.min, i);
    const float maxAxis = vec3_component(box.max, i);
    // Only an exactly-zero component is parallel to the slab. A magnitude
    // cutoff here (formerly 1e-8) silently rejected legitimate tiny
    // components — the shape every large-scale inverse transform produces —
    // turning far hits into misses; IEEE division handles those components
    // exactly (a reciprocal overflowing to infinity still rejects outside
    // origins via tMin > tMax and passes inside ones).
    if (directionAxis == 0.0F) {
      if ((originAxis < minAxis) || (originAxis > maxAxis)) {
        return false;
      }
    } else {
      const float invD = 1.0F / directionAxis;
      float t0 = (minAxis - originAxis) * invD;
      float t1 = (maxAxis - originAxis) * invD;
      if (t0 > t1) {
        const float tmp = t0;
        t0 = t1;
        t1 = tmp;
      }
      tMin = (t0 > tMin) ? t0 : tMin;
      tMax = (t1 < tMax) ? t1 : tMax;
      if (tMin > tMax) {
        return false;
      }
    }
  }

  if (outT != nullptr) {
    *outT = tMin;
  }
  return true;
}

} // namespace engine::math
