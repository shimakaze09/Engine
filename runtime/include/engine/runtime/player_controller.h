// Declares player controller types and APIs for the Engine runtime world.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "engine/core/entity.h"

namespace engine::runtime {

// Maps input to actions on the controlled entity. One per player.
// The PlayerController owns the mapping between a player slot and the entity
// they control, plus an optional name tag for identification.
struct PlayerController final {
  static constexpr std::size_t kMaxPlayers = 4U;
  static constexpr std::size_t kMaxNameLength = 32U;

  char name[kMaxNameLength] = {};
  core::Entity controlledEntity = core::kInvalidEntity;
  std::uint8_t playerIndex = 0U;
  bool active = false;

  /// Resets this object back to its reusable empty state.
  void reset() noexcept {
    name[0] = '\0';
    controlledEntity = core::kInvalidEntity;
    playerIndex = 0U;
    active = false;
  }
};

// Fixed-size array of player controllers. Provides look-up and management.
struct PlayerControllerArray final {
  static constexpr std::size_t kMaxPlayers = PlayerController::kMaxPlayers;

  std::array<PlayerController, kMaxPlayers> controllers{};

  // Assign entity to a player slot. Returns false on out-of-range.
  bool set_controlled_entity(std::uint8_t playerIndex,
                             core::Entity entity) noexcept {
    if (playerIndex >= kMaxPlayers) {
      return false;
    }
    auto &pc = controllers[playerIndex];
    pc.controlledEntity = entity;
    pc.playerIndex = playerIndex;
    pc.active = (entity != core::kInvalidEntity);
    return true;
  }

  // Get the controlled entity for a player. Returns kInvalidEntity if invalid.
  core::Entity get_controlled_entity(std::uint8_t playerIndex) const noexcept {
    if (playerIndex >= kMaxPlayers) {
      return core::kInvalidEntity;
    }
    return controllers[playerIndex].controlledEntity;
  }

  // Get the script-facing entity index for a player. Returns 0 if invalid.
  std::uint32_t
  get_controlled_entity_index(std::uint8_t playerIndex) const noexcept {
    return get_controlled_entity(playerIndex).index;
  }

  // Get a pointer to the PlayerController for a slot. Returns nullptr if OOB.
  const PlayerController *get(std::uint8_t playerIndex) const noexcept {
    if (playerIndex >= kMaxPlayers) {
      return nullptr;
    }
    return &controllers[playerIndex];
  }

  /// Returns the requested value.
  PlayerController *get(std::uint8_t playerIndex) noexcept {
    if (playerIndex >= kMaxPlayers) {
      return nullptr;
    }
    return &controllers[playerIndex];
  }

  // Clear entity reference for a player when that entity is destroyed.
  void on_entity_destroyed(core::Entity entity) noexcept {
    for (auto &pc : controllers) {
      if (pc.controlledEntity == entity) {
        pc.controlledEntity = core::kInvalidEntity;
        pc.active = false;
      }
    }
  }

  // Reset all player controllers.
  void reset() noexcept {
    for (auto &pc : controllers) {
      /// Resets this object back to its reusable empty state.
      pc.reset();
    }
  }
};

} // namespace engine::runtime
