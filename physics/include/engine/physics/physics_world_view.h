#pragma once

// Abstract interface exposing the subset of World that the physics module
// needs. runtime::World implements this; physics code programs against the
// interface so it has no compile-time dependency on the runtime module.

#include <cstddef>
#include <cstdint>

#include "engine/physics/physics_context.h"
#include "engine/physics/physics_types.h"

namespace engine::physics {

class PhysicsWorldView {
public:
  // --- Simulation access token -----------------------------------------------
  // Opaque proof that we are in the Simulation phase; required to obtain
  // writable transform pointers.
  class SimulationAccessToken final {
  public:
    constexpr bool valid() const noexcept { return m_valid; }

  private:
    explicit constexpr SimulationAccessToken(bool isValid) noexcept
        : m_valid(isValid) {}
    bool m_valid = false;
    friend class PhysicsWorldView;
  };

  virtual ~PhysicsWorldView() = default;

  // --- Transforms -----------------------------------------------------------
  virtual std::size_t transform_count() const noexcept = 0;
  virtual bool get_transform(Entity entity,
                             Transform *outTransform) const noexcept = 0;
  virtual bool
  get_transform_update_range(std::size_t startIndex, std::size_t count,
                             const Entity **outEntities,
                             const Transform **outReadTransforms,
                             Transform **outWriteTransforms) noexcept = 0;
  virtual SimulationAccessToken simulation_access_token() const noexcept = 0;
  virtual Transform *
  get_transform_write_ptr(Entity entity,
                          const SimulationAccessToken &token) noexcept = 0;

  // --- Colliders ------------------------------------------------------------
  virtual std::size_t collider_count() const noexcept = 0;
  virtual bool
  get_collider_range(std::size_t startIndex, std::size_t count,
                     const Entity **outEntities,
                     const Collider **outColliders) const noexcept = 0;
  virtual const Collider *get_collider_ptr(Entity entity) const noexcept = 0;

  // --- Rigid bodies ---------------------------------------------------------
  virtual RigidBody *get_rigid_body_ptr(Entity entity) noexcept = 0;
  virtual const RigidBody *get_rigid_body_ptr(Entity entity) const noexcept = 0;
  virtual bool get_rigid_body(Entity entity,
                              RigidBody *outRigidBody) const noexcept = 0;

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
