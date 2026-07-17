// Axis-aligned bounding box and its containment/overlap helpers, defined
// inline for broadphase and culling hot paths.

#pragma once

#include "engine/math/vec3.h"

namespace engine::math {

/// Axis-aligned bounding box spanning [min, max] per axis.
struct AABB final {
  Vec3 min = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 max = Vec3(0.0F, 0.0F, 0.0F);
};

/// Whether the point lies inside or on the box boundary.
constexpr bool aabb_contains(const AABB &box, const Vec3 &point) noexcept {
  return (point.x >= box.min.x) && (point.x <= box.max.x) &&
         (point.y >= box.min.y) && (point.y <= box.max.y) &&
         (point.z >= box.min.z) && (point.z <= box.max.z);
}

/// Whether two boxes overlap (touching counts as overlap).
constexpr bool aabb_intersects(const AABB &a, const AABB &b) noexcept {
  return (a.min.x <= b.max.x) && (a.max.x >= b.min.x) &&
         (a.min.y <= b.max.y) && (a.max.y >= b.min.y) &&
         (a.min.z <= b.max.z) && (a.max.z >= b.min.z);
}

/// Smallest box enclosing both inputs.
constexpr AABB aabb_union(const AABB &a, const AABB &b) noexcept {
  AABB result{};
  result.min.x = (a.min.x < b.min.x) ? a.min.x : b.min.x;
  result.min.y = (a.min.y < b.min.y) ? a.min.y : b.min.y;
  result.min.z = (a.min.z < b.min.z) ? a.min.z : b.min.z;
  result.max.x = (a.max.x > b.max.x) ? a.max.x : b.max.x;
  result.max.y = (a.max.y > b.max.y) ? a.max.y : b.max.y;
  result.max.z = (a.max.z > b.max.z) ? a.max.z : b.max.z;
  return result;
}

/// Box centered at `center` extending `halfExtents` along each axis.
constexpr AABB aabb_from_center_half_extents(const Vec3 &center,
                                             const Vec3 &halfExtents) noexcept {
  return AABB{Vec3(center.x - halfExtents.x, center.y - halfExtents.y,
                   center.z - halfExtents.z),
              Vec3(center.x + halfExtents.x, center.y + halfExtents.y,
                   center.z + halfExtents.z)};
}

/// Geometric center of the box.
constexpr Vec3 aabb_center(const AABB &box) noexcept {
  return Vec3((box.min.x + box.max.x) * 0.5F, (box.min.y + box.max.y) * 0.5F,
              (box.min.z + box.max.z) * 0.5F);
}

/// Half of the box size along each axis.
constexpr Vec3 aabb_half_extents(const AABB &box) noexcept {
  return Vec3((box.max.x - box.min.x) * 0.5F, (box.max.y - box.min.y) * 0.5F,
              (box.max.z - box.min.z) * 0.5F);
}

} // namespace engine::math
