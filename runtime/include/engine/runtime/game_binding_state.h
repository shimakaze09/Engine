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

/// The state the Lua game bindings read and write; default-construction is
/// a full reset to the documented defaults.
struct GameBindingState final {
  static constexpr std::size_t kMaxPlayerControllers = 4U;
  static_assert(kMaxPlayerControllers == PlayerControllerArray::kMaxPlayers,
                "controller-entity mirror must cover every player slot");

  char gameMode[64]{};
  char gameState[64]{};
  core::Entity playerControllerEntities[kMaxPlayerControllers]{};
  GameState persistentState{};
  PlayerControllerArray playerControllers{};

  // The label defaults are written by the constructor: MSVC left char-array
  // NSDMIs of a brace-initialized static instance zeroed (PR #242 CI lane),
  // so the defaults must not rely on that pattern.
  constexpr GameBindingState() noexcept {
    copy_label(gameMode, "default");
    copy_label(gameState, "startup");
  }

private:
  /// Bounded label copy usable in constant evaluation.
  static constexpr void copy_label(char (&dst)[64], const char *src) noexcept {
    std::size_t i = 0U;
    for (; (i < 63U) && (src[i] != '\0'); ++i) {
      dst[i] = src[i];
    }
    dst[i] = '\0';
  }
};

} // namespace engine::runtime
