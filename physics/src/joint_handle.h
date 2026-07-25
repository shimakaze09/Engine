// Defines generation-bearing joint handles for private physics implementations.

#pragma once

#include "engine/physics/physics_context.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::physics {

constexpr unsigned kJointSlotBits = 12U;
constexpr JointId kJointSlotMask = (1U << kJointSlotBits) - 1U;
constexpr std::uint32_t kJointGenerationMask =
    (std::numeric_limits<std::uint32_t>::max() >> kJointSlotBits) - 1U;

/// Advances a joint generation while reserving zero and the invalid-ID value.
inline std::uint32_t
next_joint_generation(std::uint32_t generation) noexcept {
  ++generation;
  if ((generation == 0U) || (generation > kJointGenerationMask)) {
    generation = 1U;
  }
  return generation;
}

/// Encodes an active slot and its generation as an externally visible ID.
inline JointId make_joint_id(std::size_t slotIndex,
                             std::uint32_t generation) noexcept {
  if ((slotIndex >= kMaxPhysicsJoints) || (generation == 0U) ||
      (generation > kJointGenerationMask)) {
    return kInvalidJointId;
  }
  return static_cast<JointId>(
      (generation << kJointSlotBits) | static_cast<JointId>(slotIndex));
}

/// Claims the first inactive slot and returns its generation-bearing ID.
inline JointId claim_joint_slot(PhysicsContext &context,
                                PhysicsJointSlot **outSlot) noexcept {
  if (outSlot == nullptr) {
    return kInvalidJointId;
  }
  *outSlot = nullptr;

  for (std::size_t index = 0U; index < kMaxPhysicsJoints; ++index) {
    PhysicsJointSlot &slot = context.joints[index];
    if (slot.active) {
      continue;
    }

    const std::uint32_t generation = slot.generation;
    slot = PhysicsJointSlot{};
    slot.generation = generation;
    if (index >= context.jointCount) {
      context.jointCount = index + 1U;
    }
    *outSlot = &slot;
    return make_joint_id(index, generation);
  }
  return kInvalidJointId;
}

/// Resolves an ID only when both its slot and generation are still active.
inline PhysicsJointSlot *find_joint_slot(PhysicsContext &context,
                                         JointId id,
                                         std::size_t *outIndex = nullptr) noexcept {
  if (id == kInvalidJointId) {
    return nullptr;
  }

  const std::size_t index = static_cast<std::size_t>(id & kJointSlotMask);
  const std::uint32_t generation = id >> kJointSlotBits;
  if ((index >= kMaxPhysicsJoints) || (generation == 0U)) {
    return nullptr;
  }

  PhysicsJointSlot &slot = context.joints[index];
  if (!slot.active || (slot.generation != generation)) {
    return nullptr;
  }
  if (outIndex != nullptr) {
    *outIndex = index;
  }
  return &slot;
}

/// Clears a slot and advances its generation so old IDs remain stale.
inline void retire_joint_slot(PhysicsJointSlot &slot) noexcept {
  const std::uint32_t generation = next_joint_generation(slot.generation);
  slot = PhysicsJointSlot{};
  slot.generation = generation;
}

} // namespace engine::physics
