// Defines the bounded texture-handle bit layout for renderer internals.

#pragma once

#include "engine/renderer/texture_loader.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::renderer::texture_handle_detail {

constexpr unsigned kSlotBits = 10U;
constexpr std::uint32_t kSlotMask = (1U << kSlotBits) - 1U;
constexpr std::uint32_t kGenerationMask =
    std::numeric_limits<std::uint32_t>::max() >> kSlotBits;

/// Advances within the encoded generation field and skips zero.
constexpr std::uint32_t next_generation(std::uint32_t generation) noexcept {
  return (generation >= kGenerationMask) ? 1U : (generation + 1U);
}

/// Encodes one nonzero slot and bounded generation, or returns invalid.
constexpr TextureHandle make_handle(std::size_t slotIndex,
                                    std::uint32_t generation) noexcept {
  if ((slotIndex == 0U) ||
      (slotIndex > static_cast<std::size_t>(kSlotMask)) ||
      (generation == 0U) || (generation > kGenerationMask)) {
    return kInvalidTextureHandle;
  }
  return TextureHandle{
      (generation << kSlotBits) | static_cast<std::uint32_t>(slotIndex)};
}

/// Extracts the slot field from an encoded handle.
constexpr std::uint32_t slot_index(TextureHandle handle) noexcept {
  return handle.id & kSlotMask;
}

/// Extracts the generation field from an encoded handle.
constexpr std::uint32_t generation(TextureHandle handle) noexcept {
  return handle.id >> kSlotBits;
}

} // namespace engine::renderer::texture_handle_detail
