#pragma once

#include "engine/math/aabb.h"
#include "engine/math/vec3.h"

namespace engine::math {

struct Ray final {
  Vec3 origin = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 direction = Vec3(0.0F, 0.0F, 1.0F);
};

// Returns true if the ray hits the box. Sets *outT to the hit distance (>= 0).
bool ray_intersects_aabb(const Ray &ray, const AABB &box,
                         float *outT) noexcept;

} // namespace engine::math
