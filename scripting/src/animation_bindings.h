// Declares Lua animation bindings for the scripting module: the
// engine.set_anim_param parameter API, animation-event handler
// registration, and the dispatch entry the frame pipeline calls after
// the animation update fires clip-timeline events.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: engine.set_anim_param(entity, name, value).
int lua_engine_set_anim_param(lua_State *state) noexcept;

/// Lua binding: engine.on_anim_event(callback) -> handler id.
int lua_engine_on_anim_event_register(lua_State *state) noexcept;

/// Lua binding: engine.remove_anim_event_handler(id).
int lua_engine_remove_anim_event_handler(lua_State *state) noexcept;

/// Releases all registered Lua animation-event callback refs.
void clear_anim_event_handlers(lua_State *state) noexcept;

/// Calls every registered handler plus the global on_anim_event fallback
/// with (entity, eventName) for each event the last animation update
/// fired. No-op without a Lua state or events.
void dispatch_anim_event_handlers() noexcept;

} // namespace engine::scripting
