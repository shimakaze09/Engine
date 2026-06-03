// Declares physics world view types and APIs for the Engine physics system.

#pragma once

// Abstract interface exposing the subset of World that the physics module
// needs. runtime::World implements this; physics code programs against the
// interface so it has no compile-time dependency on the runtime module.

#include <cstddef>
#include <cstdint>

#include "engine/physics/physics_context.h"
#include "engine/physics/physics_types.h"

namespace engine::physics {

/// Owns the physics world view behavior and state.
class PhysicsWorldView {
public:
  // --- Simulation access token -----------------------------------------------
  // Opaque proof that we are in the Simulation phase; required to obtain
  // writable transform pointers.
  class SimulationAccessToken final {
  /// Handles valid.
  public:
    /// Handles valid.
    constexpr bool valid() const noexcept { return m_valid; }

  /// Handles simulation access token.
  private:
    /// Handles simulation access token.
    explicit constexpr SimulationAccessToken(bool isValid) noexcept
        /// Handles m valid.
        : m_valid(isValid) {}
    bool m_valid = false;
    friend class PhysicsWorldView;
  };

  /// Handles physics world view.
  virtual ~PhysicsWorldView() = default;

  // --- Transforms -----------------------------------------------------------
  virtual std::size_t transform_count() const noexcept = 0;
  /// Returns the requested value for transform.
  virtual bool get_transform(Entity entity,
                             Transform *outTransform) const noexcept = 0;
  /// Returns the requested value for transform update range.
  virtual bool
  get_transform_update_range(std::size_t startIndex, std::size_t count,
                             const Entity **outEntities,
                             const Transform **outReadTransforms,
                             Transform **outWriteTransforms) noexcept = 0;
  /// Handles simulation access token.
  virtual SimulationAccessToken simulation_access_token() const noexcept = 0;
  /// Returns the requested value for transform write ptr.
  virtual Transform *
  get_transform_write_ptr(Entity entity,
                          const SimulationAccessToken &token) noexcept = 0;

  // --- Colliders ------------------------------------------------------------
  virtual std::size_t collider_count() const noexcept = 0;
  /// Returns the requested value for collider range.
  virtual bool
  get_collider_range(std::size_t startIndex, std::size_t count,
                     const Entity **outEntities,
                     const Collider **outColliders) const noexcept = 0;
  /// Returns the requested value for collider ptr.
  virtual const Collider *get_collider_ptr(Entity entity) const noexcept = 0;

  // --- Rigid bodies ---------------------------------------------------------
  virtual RigidBody *get_rigid_body_ptr(Entity entity) noexcept = 0;
  /// Returns the requested value for rigid body ptr.
  virtual const RigidBody *get_rigid_body_ptr(Entity entity) const noexcept = 0;
  /// Returns the requested value for rigid body.
  virtual bool get_rigid_body(Entity entity,
                              RigidBody *outRigidBody) const noexcept = 0;

  // --- Movement authority ---------------------------------------------------
  virtual MovementAuthority
  movement_authority(Entity entity) const noexcept = 0;

  // --- Physics context (gravity, joints, collision pairs) -------------------
  virtual PhysicsContext &physics_context() noexcept = 0;
  /// Handles physics context.
  virtual const PhysicsContext &physics_context() const noexcept = 0;

/// Handles make token.
protected:
  // Factory for derived classes to create tokens.
  static constexpr SimulationAccessToken make_token(bool valid) noexcept {
    return SimulationAccessToken{valid};
  }
};

} // namespace engine::physics
