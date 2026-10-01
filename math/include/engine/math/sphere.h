// Bounding sphere with containment, sphere/sphere, and ray/sphere tests,
// defined inline for physics query hot paths.

#pragma once

#include <cmath>

#include "engine/math/ray.h"
#include "engine/math/vec3.h"

namespace engine::math {

/// Bounding sphere.
struct Sphere final {
  Vec3 center = Vec3(0.0F, 0.0F, 0.0F);
  float radius = 0.5F;
};

/// Whether the point lies inside or on the sphere.
constexpr bool sphere_contains(const Sphere &sphere,
                               const Vec3 &point) noexcept {
  return length_sq(sub(point, sphere.center)) <=
         (sphere.radius * sphere.radius);
}

/// Whether two spheres overlap (touching counts as overlap).
constexpr bool sphere_intersects_sphere(const Sphere &a,
                                        const Sphere &b) noexcept {
  const float radiusSum = a.radius + b.radius;
  return length_sq(sub(b.center, a.center)) <= (radiusSum * radiusSum);
}

/// Nearest non-negative ray/sphere intersection; false when the ray misses or
/// the sphere is entirely behind the origin. *outT is the parametric t along
/// ray.direction (origin + t * direction is the hit point); the physical
/// distance is t * length(direction) and equals t only for unit directions.
inline bool ray_intersects_sphere(const Ray &ray, const Sphere &sphere,
                                  float *outT) noexcept {
  const Vec3 oc = sub(ray.origin, sphere.center);
  const float a = dot(ray.direction, ray.direction);
  // Degenerate only when the squared length computes to exactly zero (a
  // true zero direction, or one so small the square underflows past float's
  // subnormal range). A magnitude cutoff here (formerly 1e-12) rejected
  // legitimate tiny directions — the shape a large-scale inverse transform
  // produces — turning far hits into misses; the quadratic is homogeneous
  // in the direction scale, so float handles those coefficients exactly.
  if (a == 0.0F) {
    return false;
  }

  // The discriminant comes from the ray's closest approach to the centre
  // (Ray Tracing Gems, ch. 7): r^2 - |oc - (b/a) d|^2, scaled by a. The
  // textbook b^2 - 4ac subtracts two values of the order of the squared
  // distance to the sphere, so at 100 m a 5 cm sphere's miss margin was
  // rounding noise and rays passing beside it reported hits.
  const float b = dot(oc, ray.direction);
  const Vec3 closest = sub(oc, mul(ray.direction, b / a));
  const float discriminant =
      a * ((sphere.radius * sphere.radius) - length_sq(closest));
  if (discriminant < 0.0F) {
    return false;
  }

  // Stable roots: q takes the sign of b so the sum never cancels, and the
  // second root is c / q rather than a difference of near-equal values.
  // Direct division: a reciprocal of a overflows to infinity for tiny a
  // (turning huge finite hits into inf), while the division itself stays
  // finite whenever the true quotient is representable.
  const float c = length_sq(oc) - (sphere.radius * sphere.radius);
  const float sqrtDisc = std::sqrt(discriminant);
  const float q = -(b + std::copysign(sqrtDisc, b));
  const float rootA = q / a;
  const float rootB = (q != 0.0F) ? (c / q) : rootA;
  const float t0 = std::fmin(rootA, rootB);
  const float t1 = std::fmax(rootA, rootB);
  const float t = (t0 >= 0.0F) ? t0 : t1;

  if (t < 0.0F) {
    return false;
  }

  if (outT != nullptr) {
    *outT = t;
  }
  return true;
}

} // namespace engine::math
