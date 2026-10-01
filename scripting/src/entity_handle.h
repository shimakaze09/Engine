// Declares Lua entity-handle helpers for the Engine scripting system.

#pragma once

#include <cstdint>

#include "engine/core/entity.h"
#include "entity_handle_value.h"

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Encodes a runtime entity into Lua's numeric handle format.
bool encode_lua_entity_handle(core::Entity entity,
                              lua_Integer *outHandle) noexcept;

/// Pushes a runtime entity as a Lua handle, or nil when invalid.
void push_entity_handle(lua_State *state, core::Entity entity) noexcept;

/// Decodes a Lua stack value as an entity handle without checking liveness.
bool decode_lua_entity_handle(lua_State *state, int index,
                              core::Entity *outEntity) noexcept;

/// Reads a live entity handle from Lua, the argument every binding that
/// acts on an entity takes. A value that is not one -- not a handle, a
/// handle from before the last scene load, or one naming a destroyed
/// entity -- is refused, and the first refusal at each script line logs a
/// Warning naming the line, the binding and the reason, as Unity,
/// Godot and Unreal report a call on a destroyed object. The binding then
/// answers false or nil as before.
bool read_entity(lua_State *state, int index,
                 core::Entity *outEntity) noexcept;

/// Forgets which script lines already reported an entity argument, so a
/// new run reports them again.
void reset_entity_argument_reports() noexcept;

} // namespace engine::scripting
