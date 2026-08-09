// Declares ccd types and APIs for the Engine physics system.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/physics_types.h"

#include <cstdint>

namespace engine::physics {

class PhysicsWorldView;

/// Result of a CCD sweep for a single body.
struct CcdSweepResult {
  bool hit = false;
  float timeOfImpact = 1.0F; ///< Fraction of dt at which impact occurs [0,1].
  math::Vec3 contactPoint{};
  math::Vec3 contactNormal{};
  std::uint32_t hitEntityIndex = 0U;
  /// Hit body's snapshot velocity (zero when no snapshot vouched for it).
  math::Vec3 targetVelocity{};
  /// Pair restitution combined with the discrete solver's max rule.
  float combinedRestitution = 0.0F;
  /// Hit owner's inverse mass (zero for static or ownerless targets).
  float targetInverseMass = 0.0F;
  /// True when the hit body is fast enough for its own sweep to apply the
  /// symmetric impulse share; slow dynamic targets leave the exchange to
  /// the discrete solver so momentum is never deleted one-sidedly.
  bool targetRespondsInCcd = false;
};

/// Performs bilateral advancement for one collider owned by a moving body.
/// The collider may live on the body root or a child compound object.
///
/// @param world     The physics world view.
/// @param entity    The moving collider entity.
/// @param body      The rigid body (must have inverseMass > 0).
/// @param collider  The entity's collider.
/// @param transform The entity's current transform.
/// @param dt        The timestep in seconds.
/// @return CCD sweep result with time-of-impact if a hit was found.
CcdSweepResult bilateral_advance_ccd(const PhysicsWorldView &world,
                                     Entity entity, const RigidBody &body,
                                     const Collider &collider,
                                     const Transform &transform,
                                     float dt) noexcept;

/// Returns the CCD velocity threshold (minimum speed to trigger CCD).
/// Reads from CVar `physics.ccd_threshold`.
float ccd_velocity_threshold() noexcept;

} // namespace engine::physics
