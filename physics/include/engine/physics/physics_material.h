#pragma once

#include <cmath>

namespace engine::physics {

struct PhysicsMaterial final {
  float staticFriction = 0.5F;
  float dynamicFriction = 0.3F;
  float restitution = 0.3F;
  float density = 1.0F;
};

// Combine two materials at contact time.
// friction = sqrt(a * b), restitution = max(a, b).
inline PhysicsMaterial combine_materials(const PhysicsMaterial &a,
                                         const PhysicsMaterial &b) noexcept {
  PhysicsMaterial result;
  result.staticFriction = std::sqrt(a.staticFriction * b.staticFriction);
  result.dynamicFriction = std::sqrt(a.dynamicFriction * b.dynamicFriction);
  result.restitution =
      (a.restitution > b.restitution) ? a.restitution : b.restitution;
  result.density = 0.0F; // not meaningful for combined material
  return result;
}

} // namespace engine::physics
