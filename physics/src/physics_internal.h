// Declares module-private helpers shared across the physics TUs: shape
// payload lookups and the tuning values used by more than one stage.

#pragma once

#include "engine/physics/physics_context.h"

namespace engine::physics {

/// Bodies below this energy for kSleepFramesRequired frames go to sleep;
/// contacts wake a sleeper only when the other body exceeds it.
constexpr float kSleepThreshold = 0.01F;

/// Sign of value, treating zero as positive.
inline float sign_or_positive(float value) noexcept {
  return (value < 0.0F) ? -1.0F : 1.0F;
}

/// Entity's convex-hull payload slot, or nullptr when none is set.
ConvexHullData *find_hull_data(PhysicsContext &context,
                               Entity entity) noexcept;
/// Const overload of find_hull_data.
const ConvexHullData *find_hull_data(const PhysicsContext &context,
                                     Entity entity) noexcept;
/// Entity's heightfield payload slot, or nullptr when none is set.
HeightfieldData *find_heightfield_data(PhysicsContext &context,
                                       Entity entity) noexcept;
/// Const overload of find_heightfield_data.
const HeightfieldData *find_heightfield_data(const PhysicsContext &context,
                                             Entity entity) noexcept;

} // namespace engine::physics
