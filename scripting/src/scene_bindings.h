// Declares Lua scene management bindings for the Engine scripting system.

#pragma once

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Registers scene save/load/new functions on the engine Lua table.
void register_scene_bindings(lua_State *state) noexcept;

/// Clears any pending scene operation requested from Lua.
void reset_scene_bindings() noexcept;

/// Marks that a scene-transition on_end_play dispatch (#198) is running so
/// a handler's own load_scene/new_scene request is rejected with a logged
/// warning instead of silently corrupting the transition already
/// committing. Called only from
/// dispatch_entity_scripts_end_for_transition().
void begin_scene_teardown_dispatch() noexcept;
/// Ends the reentrancy window opened by begin_scene_teardown_dispatch.
void end_scene_teardown_dispatch() noexcept;

} // namespace engine::scripting
