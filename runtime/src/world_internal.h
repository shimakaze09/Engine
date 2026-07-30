// Declares World-private helpers shared by the world_* translation units:
// name-lookup slot states, local-to-world seeding, and error logging.

#pragma once

#include <cstdint>
#include <cstdio>

#include "engine/core/logging.h"
#include "engine/math/transform.h"
#include "engine/runtime/world.h"

namespace engine::runtime {

constexpr std::uint8_t kNameSlotEmpty = 0U;
constexpr std::uint8_t kNameSlotOccupied = 1U;
constexpr std::uint8_t kNameSlotTombstone = 2U;

/// Seeds a world transform (and cached matrix) from a local transform.
inline WorldTransform world_transform_from_local(
    const Transform &local) noexcept {
  WorldTransform world{};
  world.position = local.position;
  world.rotation = local.rotation;
  world.scale = local.scale;
  world.matrix = math::compose_trs(world.position, world.rotation, world.scale);
  return world;
}

/// Formats and logs one world component-API failure.
inline void log_component_error(const char *label,
                                const char *reason) noexcept {
  char message[128] = {};
  std::snprintf(message, sizeof(message), "%s %s", label, reason);
  core::log_message(core::LogLevel::Error, "world", message);
}

} // namespace engine::runtime
