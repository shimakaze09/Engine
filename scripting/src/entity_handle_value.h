// Declares the Lua-independent numeric entity-handle codec (index +
// generation + bound-world content epoch packed into a positive 63-bit
// value) so tests and non-Lua callers can exercise handle validation
// without pulling in Lua headers.

#pragma once

#include <cstdint>

#include "engine/runtime/world.h"

namespace engine::scripting {

/// Encodes an entity plus the bound world's content epoch into the
/// numeric handle format; false when any field exceeds its bit budget or
/// the entity is not alive in the currently bound world.
bool encode_entity_handle_value(runtime::Entity entity,
                                std::uint64_t *outHandle) noexcept;

/// Decodes Lua's numeric entity handle format without checking liveness;
/// false when malformed or when the handle's content epoch does not match
/// the bound world (a retained handle from replaced world contents).
bool decode_entity_handle_value(std::uint64_t rawHandle,
                                runtime::Entity *outEntity) noexcept;

} // namespace engine::scripting
