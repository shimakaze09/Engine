// Declares the reset of the run-scoped game-state label the Lua bindings
// act on.

#pragma once

namespace engine::scripting {

/// Restores the bound game-state label to its default.
void reset_game_bindings() noexcept;

} // namespace engine::scripting
