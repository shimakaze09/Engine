// Declares shared Lua binding helpers (argument parsing and error logging)
// used across the scripting module's binding translation units.

#pragma once

struct lua_State;

#include "engine/math/vec3.h"

namespace engine::scripting {

/// Reads three consecutive number args starting at startIndex into a Vec3.
bool read_vec3_args(lua_State *state, int startIndex,
                    math::Vec3 *outVec) noexcept;

/// Logs the Lua error on top of the stack with a traceback, then pops it.
void log_lua_error(const char *context) noexcept;

} // namespace engine::scripting
