// Declares aabb types and APIs for the Engine math library.

#pragma once

#include "engine/math/vec3.h"

namespace engine::math {

/// Stores aabb data used by the engine.
struct AABB final {
  Vec3 min = Vec3(0.0F, 0.0F, 0.0F);
  Vec3 max = Vec3(0.0F, 0.0F, 0.0F);
};

/// Handles aabb contains.
bool aabb_contains(const AABB &box, const Vec3 &point) noexcept;
/// Handles aabb intersects.
bool aabb_intersects(const AABB &a, const AABB &b) noexcept;
/// Handles aabb union.
AABB aabb_union(const AABB &a, const AABB &b) noexcept;
/// Handles aabb from center half extents.
AABB aabb_from_center_half_extents(const Vec3 &center,
                                   const Vec3 &halfExtents) noexcept;
/// Handles aabb center.
Vec3 aabb_center(const AABB &box) noexcept;
/// Handles aabb half extents.
Vec3 aabb_half_extents(const AABB &box) noexcept;

} // namespace engine::math
