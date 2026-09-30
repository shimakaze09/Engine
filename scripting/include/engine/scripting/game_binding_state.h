// Run-scoped scripting state: the game-state label scripts set and read
// back (engine.set_game_state / get_game_state). EnginePipeline owns one
// instance per run and binds it into the scripting game bindings for the
// run's lifetime; standalone (test) use without a pipeline falls back to a
// scripting-local instance.

#pragma once

#include <cstddef>

namespace engine::scripting {

/// The state the Lua game bindings read and write; default-construction is
/// a full reset to the documented default.
struct GameBindingState final {
  char gameState[64]{};

  // The label default is written by the constructor: MSVC left char-array
  // NSDMIs of a brace-initialized static instance zeroed,
  // so the default must not rely on that pattern.
  constexpr GameBindingState() noexcept {
    const char *src = "startup";
    std::size_t i = 0U;
    while ((i < 63U) && (*src != '\0')) {
      gameState[i] = *src;
      ++src;
      ++i;
    }
    gameState[i] = '\0';
  }
};

} // namespace engine::scripting
