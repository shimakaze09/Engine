// Declares sphere types and APIs for the Engine math library.

#pragma once

#include "engine/math/vec3.h"

namespace engine::math {
/// Stores ray data used by the engine.
struct Ray;
}

namespace engine::math {

/// Stores sphere data used by the engine.
struct Sphere final {
  Vec3 center = Vec3(0.0F, 0.0F, 0.0F);
  float radius = 0.5F;
};

/// Handles sphere contains.
bool sphere_contains(const Sphere &sphere, const Vec3 &point) noexcept;
/// Handles sphere intersects sphere.
bool sphere_intersects_sphere(const Sphere &a, const Sphere &b) noexcept;
/// Handles ray intersects sphere.
bool ray_intersects_sphere(const Ray &ray, const Sphere &sphere,
                           float *outT) noexcept;

} // namespace engine::math
