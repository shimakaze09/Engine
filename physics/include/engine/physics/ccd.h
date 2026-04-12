#pragma once

#include "engine/math/vec3.h"

#include <cstdint>

namespace engine::runtime {
class World;
struct Entity;
struct RigidBody;
struct Collider;
struct Transform;
} // namespace engine::runtime

namespace engine::physics {

/// Result of a CCD sweep for a single body.
struct CcdSweepResult {
  bool hit = false;
  float timeOfImpact = 1.0F; ///< Fraction of dt at which impact occurs [0,1].
  math::Vec3 contactPoint{};
  math::Vec3 contactNormal{};
  std::uint32_t hitEntityIndex = 0U;
};

/// Perform bilateral advancement CCD for a single moving body.
/// Sweeps the body's collider from its current position along its velocity
/// for up to `dt` seconds. Returns the earliest time-of-impact against any
/// static or dynamic collider in the world.
///
/// @param world     The physics world.
/// @param entity    The moving entity.
/// @param body      The rigid body (must have inverseMass > 0).
/// @param collider  The entity's collider.
/// @param transform The entity's current transform.
/// @param dt        The timestep in seconds.
/// @return CCD sweep result with time-of-impact if a hit was found.
CcdSweepResult bilateral_advance_ccd(
    const runtime::World &world, runtime::Entity entity,
    const runtime::RigidBody &body, const runtime::Collider &collider,
    const runtime::Transform &transform, float dt) noexcept;

/// Returns the CCD velocity threshold (minimum speed to trigger CCD).
/// Reads from CVar `physics.ccd_threshold`.
float ccd_velocity_threshold() noexcept;

} // namespace engine::physics
