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

// Returns true if the ray hits the box. Sets *outT to the hit distance (>= 0).
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
    if (std::fabs(directionAxis) < 1.0e-8F) {
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
