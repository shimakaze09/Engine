// Declares entity types and APIs for the Engine core engine.

#pragma once

// Lightweight entity handle used across all engine modules.
// Lives in core so it has zero cross-module dependencies.

#include <cstdint>

namespace engine::core {

/// Stores entity data used by the engine.
struct Entity final {
  std::uint32_t index = 0U;
  std::uint32_t generation = 0U;

  /// Compares values for equality.
  friend constexpr bool operator==(const Entity &, const Entity &) = default;
};

[[maybe_unused]] inline constexpr Entity kInvalidEntity{};
using PersistentId = std::uint32_t;
inline constexpr PersistentId kInvalidPersistentId = 0U;

} // namespace engine::core
