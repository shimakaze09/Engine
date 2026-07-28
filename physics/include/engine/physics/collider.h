// Declares collider types and APIs for the Engine physics system.

#pragma once

#include "engine/math/aabb.h"
#include "engine/math/component_types.h"
#include "engine/math/mat4.h"
#include "engine/math/vec3.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

// Convex hull: up to 64 faces (plane normals + offsets) and 128 vertices.
// The AABB cache is stored in the Collider::halfExtents of the matching
// entity.  ConvexHullData payloads are stored in PhysicsContext so World
// lifetime owns non-primitive collider data.
struct ConvexHullData final {
  static constexpr std::size_t kMaxPlanes = 64U;
  static constexpr std::size_t kMaxVertices = 128U;

  /// Plane as unit normal + signed distance (hull face representation).
  struct Plane final {
    math::Vec3 normal{};
    float distance = 0.0F;
  };

  std::array<Plane, kMaxPlanes> planes{};
  std::size_t planeCount = 0U;

  std::array<math::Vec3, kMaxVertices> vertices{};
  std::size_t vertexCount = 0U;

  // Cached local-space AABB (half-extents from centroid).
  math::Vec3 localCenter{};
  math::Vec3 localHalfExtents{};
};

// Heightfield: uniform grid of height samples.  Row-major layout.
// x-axis → column, z-axis → row.
struct HeightfieldData final {
  static constexpr std::size_t kMaxResolution = 129U; // (power-of-two + 1)
  static constexpr std::size_t kMaxSamples = kMaxResolution * kMaxResolution;

  std::array<float, kMaxSamples> heights{};
  std::size_t rows = 0U;    // z
  std::size_t columns = 0U; // x
  float spacingX = 1.0F;
  float spacingZ = 1.0F;
  float minY = 0.0F;
  float maxY = 0.0F;
};

// Captures a collider's validated affine transform and conservative world
// bounds.
struct ColliderWorldGeometry final {
  math::Mat4 localToWorld{};
  math::Mat4 worldToLocal{};
  math::Vec3 center{};
  math::AABB worldAabb{};
  math::ColliderShape shape = math::ColliderShape::AABB;
  math::Vec3 halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
  const ConvexHullData *convexHull = nullptr;
};

// Builds authoritative world-space collider geometry from an entity transform.
[[nodiscard]] bool
make_collider_world_geometry(const math::Collider &collider,
                             const math::Mat4 &entityWorldMatrix,
                             const ConvexHullData *convexHull,
                             ColliderWorldGeometry *outGeometry) noexcept;

// Returns the farthest world-space point in a direction for validated geometry.
[[nodiscard]] math::Vec3
collider_support_point(const ColliderWorldGeometry &geometry,
                       const math::Vec3 &worldDirection) noexcept;

} // namespace engine::physics
