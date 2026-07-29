// Implements canonical convex-hull builders for the built-in shape
// primitives.

#include "engine/physics/primitive_hulls.h"

#include <cmath>
#include <cstddef>

#include "engine/math/vec3.h"
#include "engine/physics/convex_hull.h"

namespace engine::physics {

bool build_cylinder_hull(ConvexHullData *outHull) noexcept {
  if (outHull == nullptr) {
    return false;
  }

  // Dimensions mirror build_cylinder_mesh in mesh_primitives.cpp; the hull
  // uses 16 slices (32 points, ~60 triangulated planes) so Quickhull stays
  // inside ConvexHullData::kMaxPlanes.
  constexpr int kSlices = 16;
  constexpr float kRadius = 0.5F;
  constexpr float kHalfHeight = 0.5F;
  constexpr float kTwoPi = 6.28318530718F;

  math::Vec3 points[2 * kSlices]{};
  for (int slice = 0; slice < kSlices; ++slice) {
    const float phi =
        (kTwoPi * static_cast<float>(slice)) / static_cast<float>(kSlices);
    const float x = std::cos(phi) * kRadius;
    const float z = std::sin(phi) * kRadius;
    points[slice] = math::Vec3(x, -kHalfHeight, z);
    points[kSlices + slice] = math::Vec3(x, kHalfHeight, z);
  }

  return build_convex_hull(points, 2U * kSlices, *outHull);
}

bool build_pyramid_hull(ConvexHullData *outHull) noexcept {
  if (outHull == nullptr) {
    return false;
  }

  // Vertices mirror build_pyramid_mesh in mesh_primitives.cpp exactly.
  constexpr float kBaseZBack = -0.288675F;
  constexpr float kBaseZFront = 0.577350F;
  const math::Vec3 points[4] = {
      math::Vec3(0.0F, 0.5F, 0.0F),
      math::Vec3(-0.5F, -0.5F, kBaseZBack),
      math::Vec3(0.5F, -0.5F, kBaseZBack),
      math::Vec3(0.0F, -0.5F, kBaseZFront),
  };

  return build_convex_hull(points, 4U, *outHull);
}

} // namespace engine::physics
