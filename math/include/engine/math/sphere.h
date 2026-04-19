#pragma once

#include "engine/math/vec3.h"

namespace engine::math {
struct Ray;
}

namespace engine::math {

struct Sphere final {
  Vec3 center = Vec3(0.0F, 0.0F, 0.0F);
  float radius = 0.5F;
};

bool sphere_contains(const Sphere &sphere, const Vec3 &point) noexcept;
bool sphere_intersects_sphere(const Sphere &a, const Sphere &b) noexcept;
bool ray_intersects_sphere(const Ray &ray, const Sphere &sphere,
                           float *outT) noexcept;

} // namespace engine::math
