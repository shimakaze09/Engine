#include "engine/math/sphere.h"

#include <cmath>

#include "engine/math/vec3.h"

namespace engine::math {

bool sphere_contains(const Sphere &sphere, const Vec3 &point) noexcept {
  return length_sq(sub(point, sphere.center)) <=
         (sphere.radius * sphere.radius);
}

bool sphere_intersects_sphere(const Sphere &a, const Sphere &b) noexcept {
  const float radiusSum = a.radius + b.radius;
  return length_sq(sub(b.center, a.center)) <= (radiusSum * radiusSum);
}

bool ray_intersects_sphere(const Ray &ray, const Sphere &sphere,
                           float *outT) noexcept {
  const Vec3 oc = sub(ray.origin, sphere.center);
  const float a = dot(ray.direction, ray.direction);
  if (a <= 1.0e-12F) {
    return false;
  }

  const float b = 2.0F * dot(oc, ray.direction);
  const float c = dot(oc, oc) - (sphere.radius * sphere.radius);
  const float discriminant = b * b - 4.0F * a * c;

  if (discriminant < 0.0F) {
    return false;
  }

  const float sqrtDisc = std::sqrt(discriminant);
  const float invTwoA = 1.0F / (2.0F * a);
  const float t0 = (-b - sqrtDisc) * invTwoA;
  const float t1 = (-b + sqrtDisc) * invTwoA;
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
