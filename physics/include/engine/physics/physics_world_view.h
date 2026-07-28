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

/// Interface exposing the World subset physics needs; keeps the physics
/// module free of runtime dependencies.
class PhysicsWorldView {
public:
  // --- Simulation access token -----------------------------------------------
  // Opaque proof that we are in the Simulation phase; required to obtain
  // writable transform pointers.
  class SimulationAccessToken final {
  public:
    /// True when minted during the Simulation phase.
    constexpr bool valid() const noexcept { return m_valid; }

  private:
    /// Tokens are only minted by PhysicsWorldView (see make_token).
    explicit constexpr SimulationAccessToken(bool isValid) noexcept
        : m_valid(isValid) {}
    bool m_valid = false;
    friend class PhysicsWorldView;
  };

  virtual ~PhysicsWorldView() = default;

  // --- Transforms -----------------------------------------------------------
  virtual std::size_t transform_count() const noexcept = 0;
  /// Copies the entity's local transform (read state); false when absent.
  virtual bool get_transform(Entity entity,
                             Transform *outTransform) const noexcept = 0;
  /// Copies the entity's full read-state world transform.
  virtual bool
  get_physics_transform(Entity entity,
                        PhysicsTransform *outTransform) const noexcept = 0;
  /// Read/write transform spans for one parallel update chunk.
  virtual bool
  get_transform_update_range(std::size_t startIndex, std::size_t count,
                             const Entity **outEntities,
                             const Transform **outReadTransforms,
                             Transform **outWriteTransforms) noexcept = 0;
  /// Token proving the Simulation phase is active; gates transform writes.
  virtual SimulationAccessToken simulation_access_token() const noexcept = 0;
  /// Writable transform for the entity; nullptr without a valid token.
  virtual Transform *
  get_transform_write_ptr(Entity entity,
                          const SimulationAccessToken &token) noexcept = 0;
  /// Copies the full world transform built from simulation write-state locals.
  /// Call only from the serialized collision-resolution stage, after all
  /// parallel integration jobs have completed.
  virtual bool get_simulation_physics_transform(
      Entity entity, const SimulationAccessToken &token,
      PhysicsTransform *outTransform) const noexcept = 0;

  // --- Colliders ------------------------------------------------------------
  virtual std::size_t collider_count() const noexcept = 0;
  /// Dense collider span [startIndex, startIndex+count); false out of range.
  virtual bool
  get_collider_range(std::size_t startIndex, std::size_t count,
                     const Entity **outEntities,
                     const Collider **outColliders) const noexcept = 0;
  /// Entity's collider, or nullptr when absent.
  virtual const Collider *get_collider_ptr(Entity entity) const noexcept = 0;

  // --- Rigid bodies ---------------------------------------------------------
  virtual std::size_t rigid_body_count() const noexcept = 0;
  /// Dense mutable rigid-body span used by the serialized sleep pass.
  virtual bool get_rigid_body_range(std::size_t startIndex, std::size_t count,
                                    const Entity **outEntities,
                                    RigidBody **outBodies) noexcept = 0;
  virtual RigidBody *get_rigid_body_ptr(Entity entity) noexcept = 0;
  virtual const RigidBody *get_rigid_body_ptr(Entity entity) const noexcept = 0;
  /// Copies the entity's rigid body; false when absent.
  virtual bool get_rigid_body(Entity entity,
                              RigidBody *outRigidBody) const noexcept = 0;
  /// Nearest self-or-ancestor rigid body in the stable read-state hierarchy.
  virtual Entity rigid_body_owner(Entity colliderEntity) const noexcept = 0;
  /// Nearest self-or-ancestor rigid body that owns this collider in the
  /// simulation write-state hierarchy; invalid means a static collider.
  virtual Entity
  rigid_body_owner(Entity colliderEntity,
                   const SimulationAccessToken &token) const noexcept = 0;

  // --- Movement authority ---------------------------------------------------
  virtual MovementAuthority
  movement_authority(Entity entity) const noexcept = 0;

  // --- Physics context (gravity, joints, collision pairs) -------------------
  virtual PhysicsContext &physics_context() noexcept = 0;
  virtual const PhysicsContext &physics_context() const noexcept = 0;

protected:
  // Factory for derived classes to create tokens.
  static constexpr SimulationAccessToken make_token(bool valid) noexcept {
    return SimulationAccessToken{valid};
  }
};

} // namespace engine::physics
