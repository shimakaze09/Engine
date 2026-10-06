// Declares the Lua vector and rotation values: vec2, vec3 and quat, the
// global constructors scripts build them with, and the conversions the
// bindings use to accept them. A value is an immutable userdata holding
// floats, so its arithmetic is the engine's: every operation calls the
// math library's owner of it in single precision, the precision the World
// stores, and a script that turns or moves something computes the same
// bits as engine code doing the same thing.

#pragma once

#include "engine/math/quat.h"
#include "engine/math/vec2.h"
#include "engine/math/vec3.h"

struct lua_State;

namespace engine::scripting {

/// Installs the vec2, vec3 and quat globals and their metatables.
void install_math_values(lua_State *state) noexcept;

/// Pushes a new vec2, vec3 or quat value. Raises a Lua memory error when
/// the allocator refuses, as every allocation in a binding does.
void push_vec2_value(lua_State *state, const math::Vec2 &value) noexcept;
void push_vec3_value(lua_State *state, const math::Vec3 &value) noexcept;
void push_quat_value(lua_State *state, const math::Quat &value) noexcept;

/// Reads the value at `index` when it is a vec3 (or quat), leaving `*out`
/// untouched and returning false for anything else.
bool to_vec3_value(lua_State *state, int index, math::Vec3 *out) noexcept;
bool to_quat_value(lua_State *state, int index, math::Quat *out) noexcept;

} // namespace engine::scripting
