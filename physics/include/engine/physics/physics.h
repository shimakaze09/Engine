// Declares physics types and APIs for the Engine physics system.

#pragma once

#include "engine/physics/collider.h"
#include "engine/physics/physics_types.h"

#include <cstddef>
#include <cstdint>

namespace engine::physics {

struct PhysicsContext;

/// Registers physics runtime CVars. Call after core::initialize_cvars().
bool register_physics_cvars() noexcept;

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
