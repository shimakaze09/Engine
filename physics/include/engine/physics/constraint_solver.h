// Declares constraint solver types and APIs for the Engine physics system.

#pragma once

#include "engine/math/vec3.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::physics {

class PhysicsWorldView;

// ------ Contact Manifold ---------------------------------------------------

struct PhysicsContext;

struct ManifoldContact final {
  math::Vec3 pointOnA{};
  math::Vec3 pointOnB{};
  math::Vec3 normal{};
  float penetration = 0.0F;
  float accumulatedNormalImpulse = 0.0F;
  std::uint32_t featureId = 0U;
};

/// Persistent contact set for one collider-entity pair (4 warm-started
/// points). Keyed by full Entity (index AND generation) so a destroyed and
/// reused entity index can never inherit a stale manifold.
struct ContactManifold final {
  static constexpr std::size_t kMaxContacts = 4U;
  Entity entityA = kInvalidEntity;
  Entity entityB = kInvalidEntity;
  ManifoldContact contacts[kMaxContacts]{};
  std::size_t contactCount = 0U;
  std::uint32_t lastFrameUsed = 0U;
  // Scalar inverse inertia the ORIGINATING resolve (clipped-manifold or
  // single-point) actually used for each endpoint, so issue #123's outer
  // relaxation pass re-solves the same point-relative quantity the primary
  // resolve converged instead of guessing a possibly-mismatched value
  // (clipped manifolds use a box-tensor approximation; single-point paths
  // use RigidBody::inverseInertia directly). Zero for a static or
  // non-rotating endpoint.
  float invInertiaA = 0.0F;
  float invInertiaB = 0.0F;
};

static constexpr std::size_t kMaxContactManifolds = 2048U;

/// Bucket count for the pair->manifold hash index; at most half-loaded so
/// linear probing always terminates on an empty bucket.
static constexpr std::size_t kManifoldHashBuckets = 4096U;

/// Empty sentinel for manifold hash buckets.
static constexpr std::uint32_t kManifoldSlotEmpty = 0xFFFFFFFFU;

// ------ Constraint Solver API -----------------------------------------------

// Solve all joint constraints on the world.  Called after collision resolution.
void solve_constraints(PhysicsWorldView &world, float deltaSeconds) noexcept;

// ------ Typed Joint Creation ------------------------------------------------

/// Creates a hinge with a finite nonzero axis; invalid ID on bad input/full.
JointId add_hinge_joint(PhysicsWorldView &world, Entity entityA, Entity entityB,
                        const math::Vec3 &pivot,
                        const math::Vec3 &axis) noexcept;

/// Creates a ball-socket joint at a finite pivot; invalid ID on failure.
JointId add_ball_socket_joint(PhysicsWorldView &world, Entity entityA,
                              Entity entityB, const math::Vec3 &pivot) noexcept;

/// Creates a slider along a finite nonzero axis; invalid ID on failure.
JointId add_slider_joint(PhysicsWorldView &world, Entity entityA,
                         Entity entityB, const math::Vec3 &axis) noexcept;

/// Creates a spring with finite nonnegative parameters; invalid ID on
/// failure.
JointId add_spring_joint(PhysicsWorldView &world, Entity entityA,
                         Entity entityB, float restLength, float stiffness,
                         float damping) noexcept;

/// Creates a fixed joint between distinct live endpoints; invalid ID on failure.
JointId add_fixed_joint(PhysicsWorldView &world, Entity entityA,
                        Entity entityB) noexcept;

/// Sets finite ordered limits on a live hinge or slider; false (issue #126)
/// on a stale/invalid id, wrong joint type, or out-of-range limits, with the
/// joint left unchanged. Hinge limits are twist radians and must lie within
/// [-pi, pi] (the wrapped twist measurement cannot enforce multi-turn
/// ranges).
bool set_joint_limits(PhysicsWorldView &world, JointId id, float minLimit,
                      float maxLimit) noexcept;

// ------ Contact Manifold API ------------------------------------------------
// The cache is world-scoped (stored in the context's shape store), never a
// process-global: separate worlds must not share warm-start state.

// Finds or creates the pair's persistent manifold and stamps it used in
// `frameNumber`. A stored pair matched in flipped order reinitializes empty
// (dense reorder swapped the perspective). Returns nullptr when the store
// is unavailable or full with no evictable slot.
ContactManifold *manifold_acquire(PhysicsContext &context, Entity entityA,
                                  Entity entityB,
                                  std::uint32_t frameNumber) noexcept;

// Update the manifold store with a new contact.  Returns the manifold index
// (or kMaxContactManifolds when the store is unavailable).
std::size_t manifold_add_contact(PhysicsContext &context, Entity entityA,
                                 Entity entityB, const math::Vec3 &pointOnA,
                                 const math::Vec3 &pointOnB,
                                 const math::Vec3 &normal, float penetration,
                                 std::uint32_t featureId,
                                 std::uint32_t frameNumber) noexcept;

// Evict stale manifolds (not used in `frameNumber`).
void manifold_evict_stale(PhysicsContext &context,
                          std::uint32_t frameNumber) noexcept;

// Get current manifold count (for testing).
std::size_t manifold_count(const PhysicsContext &context) noexcept;

// Reset all manifolds (for testing).
void manifold_reset(PhysicsContext &context) noexcept;

// Get manifold by index (for testing).
const ContactManifold *manifold_get(const PhysicsContext &context,
                                    std::size_t index) noexcept;

} // namespace engine::physics
