// Declares the rebinding of the Lua math table's transcendentals to the
// engine's deterministic scalar set. Lua's own library calls the C
// library's sin, cos, tan, asin, acos, atan, exp and log, which round
// differently on glibc, UCRT and libSystem, so a script that computes a
// position or a force with them writes a platform-dependent value into
// the World. The engine owns the VM, so the table is rebound at creation
// and a script cannot reach the libm versions.

#pragma once

struct lua_State;

namespace engine::scripting {

/// Replaces math.sin, cos, tan, asin, acos, atan, exp and log in the
/// global `math` table with the deterministic set. Each evaluates in
/// single precision (the World stores floats) and returns that value as
/// a Lua number. sqrt, floor, ceil, fmod, modf, abs and the integer
/// functions are already exact and stay as they are; the `^` operator
/// and math.pow, when present, still route through the C library's pow.
void install_deterministic_math(lua_State *state) noexcept;

} // namespace engine::scripting
