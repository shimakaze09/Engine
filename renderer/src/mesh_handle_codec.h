// Defines the bounded mesh-handle bit layout for renderer internals.

#pragma once

#include "engine/renderer/command_buffer.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::renderer::mesh_handle_detail {

// kMaxSlots (GpuMeshRegistry) is 4096, so 12 bits covers every valid slot
// index (1..4095); the remaining 20 bits carry the generation counter.
constexpr unsigned kSlotBits = 12U;
constexpr std::uint32_t kSlotMask = (1U << kSlotBits) - 1U;
constexpr std::uint32_t kGenerationMask =
    std::numeric_limits<std::uint32_t>::max() >> kSlotBits;

/// Advances within the encoded generation field and skips zero.
constexpr std::uint32_t next_generation(std::uint32_t generation) noexcept {
  return (generation >= kGenerationMask) ? 1U : (generation + 1U);
}

/// Encodes one nonzero slot and bounded generation, or returns invalid.
constexpr MeshHandle make_handle(std::size_t slotIndex,
                                 std::uint32_t generation) noexcept {
  if ((slotIndex == 0U) ||
      (slotIndex > static_cast<std::size_t>(kSlotMask)) ||
      (generation == 0U) || (generation > kGenerationMask)) {
    return kInvalidMeshHandle;
  }
  return MeshHandle{
      (generation << kSlotBits) | static_cast<std::uint32_t>(slotIndex)};
}

/// Extracts the slot field from an encoded handle.
constexpr std::uint32_t slot_index(MeshHandle handle) noexcept {
  return handle.id & kSlotMask;
}

/// Extracts the generation field from an encoded handle.
constexpr std::uint32_t generation(MeshHandle handle) noexcept {
  return handle.id >> kSlotBits;
}

} // namespace engine::renderer::mesh_handle_detail
