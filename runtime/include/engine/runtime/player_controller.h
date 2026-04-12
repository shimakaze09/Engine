#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace engine::runtime {

// Maps input to actions on the controlled entity. One per player.
// The PlayerController owns the mapping between a player slot and the entity
// they control, plus an optional name tag for identification.
struct PlayerController final {
  static constexpr std::size_t kMaxPlayers = 4U;
  static constexpr std::size_t kMaxNameLength = 32U;

  char name[kMaxNameLength] = {};
  std::uint32_t controlledEntity = 0U; // entity index; 0 = none
  std::uint8_t playerIndex = 0U;
  bool active = false;

  void reset() noexcept {
    name[0] = '\0';
    controlledEntity = 0U;
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
                             std::uint32_t entityIndex) noexcept {
    if (playerIndex >= kMaxPlayers) {
      return false;
    }
    auto &pc = controllers[playerIndex];
    pc.controlledEntity = entityIndex;
    pc.playerIndex = playerIndex;
    pc.active = (entityIndex != 0U);
    return true;
  }

  // Get the controlled entity for a player. Returns 0 if invalid.
  std::uint32_t get_controlled_entity(std::uint8_t playerIndex) const noexcept {
    if (playerIndex >= kMaxPlayers) {
      return 0U;
    }
    return controllers[playerIndex].controlledEntity;
  }

  // Get a pointer to the PlayerController for a slot. Returns nullptr if OOB.
  const PlayerController *get(std::uint8_t playerIndex) const noexcept {
    if (playerIndex >= kMaxPlayers) {
      return nullptr;
    }
    return &controllers[playerIndex];
  }

  PlayerController *get(std::uint8_t playerIndex) noexcept {
    if (playerIndex >= kMaxPlayers) {
      return nullptr;
    }
    return &controllers[playerIndex];
  }

  // Clear entity reference for a player when that entity is destroyed.
  void on_entity_destroyed(std::uint32_t entityIndex) noexcept {
    for (auto &pc : controllers) {
      if (pc.controlledEntity == entityIndex) {
        pc.controlledEntity = 0U;
        pc.active = false;
      }
    }
  }

  // Reset all player controllers.
  void reset() noexcept {
    for (auto &pc : controllers) {
      pc.reset();
    }
  }
};

} // namespace engine::runtime
