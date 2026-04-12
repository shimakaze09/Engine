#pragma once

#include "engine/math/vec3.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

// Convex hull: up to 64 faces (plane normals + offsets) and 128 vertices.
// The AABB cache is stored in the Collider::halfExtents of the matching
// entity.  ConvexHullData lives in the physics module, not the runtime
// World, to keep the runtime data model simple.
struct ConvexHullData final {
  static constexpr std::size_t kMaxPlanes = 64U;
  static constexpr std::size_t kMaxVertices = 128U;

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

} // namespace engine::physics
