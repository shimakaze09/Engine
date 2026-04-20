#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::physics {

class PhysicsWorldView;

// ------ Contact Manifold ---------------------------------------------------

struct ManifoldContact final {
  math::Vec3 pointOnA{};
  math::Vec3 pointOnB{};
  math::Vec3 normal{};
  float penetration = 0.0F;
  float accumulatedNormalImpulse = 0.0F;
  std::uint32_t featureId = 0U;
};

struct ContactManifold final {
  static constexpr std::size_t kMaxContacts = 4U;
  std::uint32_t entityIndexA = 0U;
  std::uint32_t entityIndexB = 0U;
  ManifoldContact contacts[kMaxContacts]{};
  std::size_t contactCount = 0U;
  std::uint32_t lastFrameUsed = 0U;
};

static constexpr std::size_t kMaxContactManifolds = 2048U;

// ------ Constraint Solver API -----------------------------------------------

// Solve all joint constraints on the world.  Called after collision resolution.
void solve_constraints(PhysicsWorldView &world, float deltaSeconds) noexcept;

// ------ Typed Joint Creation ------------------------------------------------

JointId add_hinge_joint(PhysicsWorldView &world, Entity entityA, Entity entityB,
                        const math::Vec3 &pivot,
                        const math::Vec3 &axis) noexcept;

JointId add_ball_socket_joint(PhysicsWorldView &world, Entity entityA,
                              Entity entityB, const math::Vec3 &pivot) noexcept;

JointId add_slider_joint(PhysicsWorldView &world, Entity entityA,
                         Entity entityB, const math::Vec3 &axis) noexcept;

JointId add_spring_joint(PhysicsWorldView &world, Entity entityA,
                         Entity entityB, float restLength, float stiffness,
                         float damping) noexcept;

JointId add_fixed_joint(PhysicsWorldView &world, Entity entityA,
                        Entity entityB) noexcept;

void set_joint_limits(PhysicsWorldView &world, JointId id, float minLimit,
                      float maxLimit) noexcept;

// ------ Contact Manifold API ------------------------------------------------

// Update the manifold store with a new contact.  Returns the manifold index.
std::size_t manifold_add_contact(std::uint32_t entityIndexA,
                                 std::uint32_t entityIndexB,
                                 const math::Vec3 &pointOnA,
                                 const math::Vec3 &pointOnB,
                                 const math::Vec3 &normal, float penetration,
                                 std::uint32_t featureId,
                                 std::uint32_t frameNumber) noexcept;

// Evict stale manifolds (not used in `frameNumber`).
void manifold_evict_stale(std::uint32_t frameNumber) noexcept;

// Get current manifold count (for testing).
std::size_t manifold_count() noexcept;

// Reset all manifolds (for testing).
void manifold_reset() noexcept;

// Get manifold by index (for testing).
const ContactManifold *manifold_get(std::size_t index) noexcept;

} // namespace engine::physics
