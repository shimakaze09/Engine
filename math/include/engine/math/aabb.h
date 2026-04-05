#pragma once

#include "engine/math/vec3.h"

namespace engine::math {

struct AABB final {
  Vec3 min = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 max = Vec3(0.0F, 0.0F, 0.0F);
};

bool aabb_contains(const AABB &box, const Vec3 &point) noexcept;
bool aabb_intersects(const AABB &a, const AABB &b) noexcept;
AABB aabb_union(const AABB &a, const AABB &b) noexcept;
AABB aabb_from_center_half_extents(const Vec3 &center,
                                   const Vec3 &halfExtents) noexcept;
Vec3 aabb_center(const AABB &box) noexcept;
Vec3 aabb_half_extents(const AABB &box) noexcept;

} // namespace engine::math
