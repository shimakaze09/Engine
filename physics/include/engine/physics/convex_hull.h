#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/collider.h"

#include <cstddef>

namespace engine::physics {

// Build a convex hull from a set of input points using the Quickhull
// algorithm.  Writes faces (planes) and hull vertices into `outHull`.
// Returns true on success.  Fails if fewer than 4 non-degenerate points.
bool build_convex_hull(const math::Vec3 *points, std::size_t pointCount,
                       ConvexHullData &outHull) noexcept;

// ------------- GJK / EPA ---------------------------------------------------

struct GjkResult final {
  bool intersecting = false;
  // If intersecting, EPA populates these:
  math::Vec3 normal{};       // penetration direction (A→B)
  float depth = 0.0F;        // penetration depth
  math::Vec3 contactPoint{}; // approximate contact point on the boundary
};

// GJK support function pointer type.
// Given a direction, return the farthest point of the shape along that
// direction.
using SupportFn = math::Vec3 (*)(const void *shapeData,
                                 const math::Vec3 &center,
                                 const math::Vec3 &direction) noexcept;

// Run GJK intersection test followed by EPA for penetration.
// shapeA/shapeB are opaque pointers passed to supportA/supportB.
GjkResult gjk_epa(const void *shapeA, const math::Vec3 &centerA,
                  SupportFn supportA, const void *shapeB,
                  const math::Vec3 &centerB, SupportFn supportB) noexcept;

// ------------- Support functions for built-in shapes -----------------------

math::Vec3 support_convex_hull(const void *data, const math::Vec3 &center,
                               const math::Vec3 &dir) noexcept;

math::Vec3 support_sphere(const void *data, const math::Vec3 &center,
                          const math::Vec3 &dir) noexcept;

math::Vec3 support_capsule(const void *data, const math::Vec3 &center,
                           const math::Vec3 &dir) noexcept;

math::Vec3 support_aabb(const void *data, const math::Vec3 &center,
                        const math::Vec3 &dir) noexcept;

} // namespace engine::physics
