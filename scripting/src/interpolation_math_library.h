// Declares the interpolation and smoothing functions added to the Lua math
// table: math.lerp, clamp, smoothstep, move_towards, smooth_damp,
// exp_decay, the angle helpers and their kin. Each calls the C++ owner in
// engine/math/interpolation.h in single precision, so a script's blend
// follows the same law, and produces the same bits, as engine code's.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Adds the interpolation functions to the global `math` table.
void install_interpolation_math(lua_State *state) noexcept;

} // namespace engine::scripting
