// Declares the contact-pair material combine rule every physics response
// path shares (#475): friction is the geometric mean of the two surface
// coefficients, restitution the larger of the two.

#pragma once

#include <algorithm>
#include <cmath>

#include "engine/math/component_types.h"

namespace engine::physics {

/// Surface response of one contact pair, derived from both colliders.
struct ContactMaterial final {
  float restitution;
  float staticFriction;
  float dynamicFriction;
};

/// Pair restitution: the bouncier surface wins.
inline float combine_restitution(float a, float b) noexcept {
  return std::max(a, b);
}

/// Pair friction: geometric mean of the two surface coefficients.
inline float combine_friction(float a, float b) noexcept {
  return std::sqrt(a * b);
}

/// Combines two colliders' surface parameters for their contact.
inline ContactMaterial
combine_contact_materials(const math::Collider &a,
                          const math::Collider &b) noexcept {
  return ContactMaterial{
      combine_restitution(a.restitution, b.restitution),
      combine_friction(a.staticFriction, b.staticFriction),
      combine_friction(a.dynamicFriction, b.dynamicFriction)};
}

} // namespace engine::physics
