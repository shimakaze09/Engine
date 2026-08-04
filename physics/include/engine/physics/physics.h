// Declares physics types and APIs for the Engine physics system.

#pragma once

#include "engine/physics/collider.h"
#include "engine/physics/physics_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::physics {

struct PhysicsContext;
class PhysicsWorldView;

/// Registers physics runtime CVars. Call after core::initialize_cvars().
bool register_physics_cvars() noexcept;

/// Copies the physics cvars consumed during stepping into the context's
/// per-step cache. Must run once per fixed step on the serial begin-step
/// path (before any physics chunk job) so the parallel section never
/// touches the global cvar mutex.
void refresh_step_cvar_cache(PhysicsContext &context) noexcept;

// Blocked-body warning diagnostic ---------------------------------------------
/// Counters published by the blocked-body warning (physics.blocked_warn_steps):
/// total warnings emitted plus the blocked entity and blocking-partner
/// indices from the most recent warning (0 when unknown).
struct BlockedBodyWarningStats final {
  std::uint32_t totalWarnings = 0U;
  std::uint32_t lastBlockedEntityIndex = 0U;
  std::uint32_t lastBlockingEntityIndex = 0U;
};
/// Returns the world's blocked-body warning counters.
BlockedBodyWarningStats
blocked_body_warning_stats(const PhysicsWorldView &world) noexcept;
/// Opaque generation-bearing joint identifier; never use it as a slot index.

// Runaway guards --------------------------------------------------------------
// Numeric-runaway caps shared by ingress validation and the solver; they
// exist to stop divergence, not to art-direct motion. Contact resolution
// clamps angular speed to kMaxAngularSpeed after every impulse.
// kMaxLinearSpeed is the matching linear guard (~mach 1.5, 8.3 m per fixed
// step — far above gameplay speeds while keeping broad-phase velocity
// expansion and CCD travel bounded), applied at rigid-body ingress and
// after velocity integration. kMaxInverseInertia bounds the scalar inverse
// inertia that scales lever^2 effective-mass terms and angular impulses
// (a 1 kg body of 2 cm radius is ~6e3, comfortably inside the bound).
constexpr float kMaxAngularSpeed = 12.0F;
constexpr float kMaxLinearSpeed = 500.0F;
constexpr float kMaxInverseInertia = 1.0e4F;

// Joints / Constraints --------------------------------------------------------
using JointId = std::uint32_t;
constexpr JointId kInvalidJointId = 0xFFFFFFFFU;

// Collision callbacks ---------------------------------------------------------
// pairData points to an array of [entityIndexA, entityIndexB, ...] uint32
// pairs. pairCount is the number of pairs (not the element count).
using CollisionDispatchFn = void (*)(const std::uint32_t *pairs,
                                     std::size_t pairCount) noexcept;

/// Sets the requested value for convex hull payload data.
bool set_convex_hull_data(PhysicsContext &context, Entity entity,
                          const ConvexHullData &hull) noexcept;
/// Returns convex hull payload data from the requested context.
const ConvexHullData *
get_convex_hull_data(const PhysicsContext &context, Entity entity) noexcept;
/// Returns convex hull payload data for support-function callers.
const ConvexHullData *get_hull_data_ptr(const PhysicsContext &context,
                                        Entity entity) noexcept;
/// Removes non-primitive shape payload data for an entity.
void remove_shape_payloads(PhysicsContext &context, Entity entity) noexcept;
/// Sets the requested value for heightfield payload data.
bool set_heightfield_data(PhysicsContext &context, Entity entity,
                          const HeightfieldData &heightfield) noexcept;
/// Returns heightfield payload data from the requested context.
const HeightfieldData *
get_heightfield_data(const PhysicsContext &context, Entity entity) noexcept;

} // namespace engine::physics
