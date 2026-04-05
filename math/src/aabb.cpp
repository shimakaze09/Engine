#include "engine/math/aabb.h"

#include <cmath>

namespace engine::math {

bool aabb_contains(const AABB &box, const Vec3 &point) noexcept {
  return (point.x >= box.min.x) && (point.x <= box.max.x)
         && (point.y >= box.min.y) && (point.y <= box.max.y)
         && (point.z >= box.min.z) && (point.z <= box.max.z);
}

bool aabb_intersects(const AABB &a, const AABB &b) noexcept {
  return (a.min.x <= b.max.x) && (a.max.x >= b.min.x)
         && (a.min.y <= b.max.y) && (a.max.y >= b.min.y)
         && (a.min.z <= b.max.z) && (a.max.z >= b.min.z);
}

AABB aabb_union(const AABB &a, const AABB &b) noexcept {
  AABB result{};
  result.min.x = (a.min.x < b.min.x) ? a.min.x : b.min.x;
  result.min.y = (a.min.y < b.min.y) ? a.min.y : b.min.y;
  result.min.z = (a.min.z < b.min.z) ? a.min.z : b.min.z;
  result.max.x = (a.max.x > b.max.x) ? a.max.x : b.max.x;
  result.max.y = (a.max.y > b.max.y) ? a.max.y : b.max.y;
  result.max.z = (a.max.z > b.max.z) ? a.max.z : b.max.z;
  return result;
}

AABB aabb_from_center_half_extents(const Vec3 &center,
                                   const Vec3 &halfExtents) noexcept {
  return AABB{Vec3(center.x - halfExtents.x, center.y - halfExtents.y,
                   center.z - halfExtents.z),
              Vec3(center.x + halfExtents.x, center.y + halfExtents.y,
                   center.z + halfExtents.z)};
}

Vec3 aabb_center(const AABB &box) noexcept {
  return Vec3((box.min.x + box.max.x) * 0.5F, (box.min.y + box.max.y) * 0.5F,
              (box.min.z + box.max.z) * 0.5F);
}

Vec3 aabb_half_extents(const AABB &box) noexcept {
  return Vec3((box.max.x - box.min.x) * 0.5F, (box.max.y - box.min.y) * 0.5F,
              (box.max.z - box.min.z) * 0.5F);
}

} // namespace engine::math
