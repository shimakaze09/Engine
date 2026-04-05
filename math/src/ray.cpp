#include "engine/math/ray.h"

#include <cmath>
#include <limits>

namespace engine::math {

bool ray_intersects_aabb(const Ray &ray, const AABB &box,
                         float *outT) noexcept {
  constexpr float kInfinity = std::numeric_limits<float>::infinity();

  float tMin = 0.0F;
  float tMax = kInfinity;

  const float *origin = &ray.origin.x;
  const float *direction = &ray.direction.x;
  const float *boxMin = &box.min.x;
  const float *boxMax = &box.max.x;

  for (int i = 0; i < 3; ++i) {
    if (std::fabs(direction[i]) < 1.0e-8F) {
      if ((origin[i] < boxMin[i]) || (origin[i] > boxMax[i])) {
        return false;
      }
    } else {
      const float invD = 1.0F / direction[i];
      float t0 = (boxMin[i] - origin[i]) * invD;
      float t1 = (boxMax[i] - origin[i]) * invD;
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
