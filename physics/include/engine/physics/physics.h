// Declares physics types and APIs for the Engine physics system.

#pragma once

#include "engine/physics/collider.h"

#include <cstddef>
#include <cstdint>

namespace engine::physics {

/// Stores physics context data used by the engine.
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
bool set_convex_hull_data(PhysicsContext &context, std::uint32_t entityIndex,
                          const ConvexHullData &hull) noexcept;
/// Returns convex hull payload data from the requested context.
const ConvexHullData *
get_convex_hull_data(const PhysicsContext &context,
                     std::uint32_t entityIndex) noexcept;
/// Returns convex hull payload data for support-function callers.
const ConvexHullData *get_hull_data_ptr(const PhysicsContext &context,
                                        std::uint32_t entityIndex) noexcept;
/// Sets the requested value for heightfield payload data.
bool set_heightfield_data(PhysicsContext &context, std::uint32_t entityIndex,
                          const HeightfieldData &heightfield) noexcept;
/// Returns heightfield payload data from the requested context.
const HeightfieldData *
get_heightfield_data(const PhysicsContext &context,
                     std::uint32_t entityIndex) noexcept;

} // namespace engine::physics
