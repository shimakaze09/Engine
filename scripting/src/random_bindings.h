// Declares the random-stream Lua bindings: engine.random, engine.random_int
// and engine.set_seed, plus the rebind that makes math.random and
// math.randomseed aliases of them so a script cannot reach an
// operating-system seeded generator by habit (docs/decisions/0019).

#pragma once

struct lua_State;

namespace engine::scripting {

/// Lua binding: engine.random() / engine.random(m) / engine.random(m, n),
/// following math.random's arities.
int lua_engine_random(lua_State *state) noexcept;
/// Lua binding: engine.random_int(m, n), the integer form named
/// explicitly for scripts that would rather not rely on arity.
int lua_engine_random_int(lua_State *state) noexcept;
/// Lua binding: engine.set_seed(seed).
int lua_engine_set_seed(lua_State *state) noexcept;

/// Registers this module's engine-table bindings; expects the table at the
/// top of the Lua stack.
void register_random_bindings(lua_State *state) noexcept;

/// Points math.random and math.randomseed at the engine stream. Separate
/// from the registration above because it edits the `math` table rather
/// than the engine one, and runs beside install_deterministic_math.
void install_engine_random_over_math(lua_State *state) noexcept;

} // namespace engine::scripting
