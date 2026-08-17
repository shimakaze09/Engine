// Run-scoped game-facing scripting state: the cross-scene persistent
// key/value store, player controller bindings, and the game mode/state
// labels. EnginePipeline owns one instance per run (#168 M3) and binds it
// into the scripting game bindings for the run's lifetime; standalone
// (test) use without a pipeline falls back to a scripting-local instance.

#pragma once

#include <cstddef>

#include "engine/core/entity.h"
#include "engine/runtime/game_state.h"
#include "engine/runtime/player_controller.h"

namespace engine::runtime {

/// The state the Lua game bindings read and write; plain aggregate so a
/// default-construction is a full reset to the documented defaults.
struct GameBindingState final {
  static constexpr std::size_t kMaxPlayerControllers = 4U;
  static_assert(kMaxPlayerControllers == PlayerControllerArray::kMaxPlayers,
                "controller-entity mirror must cover every player slot");

  char gameMode[64] = "default";
  char gameState[64] = "startup";
  core::Entity playerControllerEntities[kMaxPlayerControllers]{};
  GameState persistentState{};
  PlayerControllerArray playerControllers{};
};

} // namespace engine::runtime
