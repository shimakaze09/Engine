// Declares Lua entity-handle helpers for the Engine scripting system.

#pragma once

#include <cstdint>

#include "engine/runtime/world.h"

extern "C" {
#include "lua.h"
}

namespace engine::scripting {

/// Encodes a runtime entity into Lua's numeric handle format.
bool encode_lua_entity_handle(runtime::Entity entity,
                              lua_Integer *outHandle) noexcept;

/// Pushes a runtime entity as a Lua handle, or nil when invalid.
void push_entity_handle(lua_State *state, runtime::Entity entity) noexcept;

/// Returns the current live entity for an index, or invalid.
runtime::Entity entity_from_index(std::uint32_t entityIndex) noexcept;

/// Pushes the current live entity for an index as a Lua handle.
void push_entity_handle_from_index(lua_State *state,
                                   std::uint32_t entityIndex) noexcept;

/// Decodes Lua's numeric entity handle format without checking liveness.
bool decode_entity_handle_value(std::uint64_t rawHandle,
                                runtime::Entity *outEntity) noexcept;

/// Decodes a Lua stack value as an entity handle without checking liveness.
bool decode_lua_entity_handle(lua_State *state, int index,
                              runtime::Entity *outEntity) noexcept;

/// Reads a live entity handle from Lua.
bool read_entity(lua_State *state, int index,
                 runtime::Entity *outEntity) noexcept;

} // namespace engine::scripting
