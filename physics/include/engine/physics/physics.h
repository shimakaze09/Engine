#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/math/vec3.h"

namespace engine::physics {

void set_gravity(float x, float y, float z) noexcept;
math::Vec3 get_gravity() noexcept;

// Joints / Constraints --------------------------------------------------------
using JointId = std::uint32_t;
constexpr JointId kInvalidJointId = 0xFFFFFFFFU;

// Collision callbacks ---------------------------------------------------------
// pairData points to an array of [entityIndexA, entityIndexB, ...] uint32
// pairs. pairCount is the number of pairs (not the element count).
using CollisionDispatchFn = void (*)(const std::uint32_t *pairs,
                                     std::size_t pairCount) noexcept;

// Register a callback to receive collision pair data each frame.
void set_collision_dispatch(CollisionDispatchFn fn) noexcept;

// Dispatch all recorded collision pairs and reset the pair buffer.
// Call once per frame on the main thread after the physics step completes.
void dispatch_collision_callbacks() noexcept;

} // namespace engine::physics
