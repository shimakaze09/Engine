#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::physics {

// Joints / Constraints --------------------------------------------------------
using JointId = std::uint32_t;
constexpr JointId kInvalidJointId = 0xFFFFFFFFU;

// Collision callbacks ---------------------------------------------------------
// pairData points to an array of [entityIndexA, entityIndexB, ...] uint32
// pairs. pairCount is the number of pairs (not the element count).
using CollisionDispatchFn = void (*)(const std::uint32_t *pairs,
                                     std::size_t pairCount) noexcept;

} // namespace engine::physics
