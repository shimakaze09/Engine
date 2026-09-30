// Implements the run-scoped game-state label the Lua bindings read and
// write, on the pipeline-owned state while one is bound.

#include "game_bindings.h"

#include <cstdio>

#include "engine/scripting/bindable_api.h"
#include "engine/scripting/game_binding_state.h"

namespace engine::scripting {
namespace {

// Pipeline-owned when bound; the fallback keeps standalone/test
// use (no pipeline) working with identical semantics.
GameBindingState g_fallbackState{};
GameBindingState *g_boundState = nullptr;

/// Returns the state instance the game bindings currently act on.
GameBindingState &binding_state() noexcept {
  return (g_boundState != nullptr) ? *g_boundState : g_fallbackState;
}

} // namespace

void reset_game_bindings() noexcept { binding_state() = GameBindingState{}; }

/// Binds the pipeline-owned state; nullptr restores the fallback.
void bind_game_state(GameBindingState *state) noexcept {
  g_boundState = state;
}

const char *bindable_get_game_state() noexcept { return binding_state().gameState; }

bool bindable_set_game_state(const char *name) noexcept {
  if (name == nullptr) {
    return false;
  }
  std::snprintf(binding_state().gameState, sizeof(binding_state().gameState),
                "%s", name);
  return true;
}

} // namespace engine::scripting
