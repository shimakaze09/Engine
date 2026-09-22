// Implements the random-stream bindings over the runtime services table.
// Every draw goes to the World's stream, so a script's randomness is part
// of the simulation state a run is reproducible from, and math.random is
// rebound to the same source rather than left on Lua's own generator.

#include "random_bindings.h"

#include "runtime_binding.h"

#include "lauxlib.h"
#include "lua.h"

namespace engine::scripting {

namespace {

/// True when the runtime is bound and has filled the random entries. An
/// unbound script gets a refusal rather than a silent zero: a draw that
/// always returns the same number is the kind of failure that surfaces as
/// a gameplay bug days later.
bool random_available() noexcept {
  return runtime_bound() &&
         (runtime_binding().services->random_double != nullptr) &&
         (runtime_binding().services->random_range != nullptr) &&
         (runtime_binding().services->seed_random != nullptr);
}

/// Draws the integer form, both bounds included.
lua_Integer draw_range(lua_Integer minimum, lua_Integer maximum) noexcept {
  return static_cast<lua_Integer>(runtime_binding().services->random_range(
      runtime_binding().world, static_cast<std::int64_t>(minimum),
      static_cast<std::int64_t>(maximum)));
}

} // namespace

int lua_engine_random(lua_State *state) noexcept {
  if (!random_available()) {
    return luaL_error(state, "engine.random needs a bound runtime");
  }
  const int arguments = lua_gettop(state);
  // The arities Lua's own math.random documents, so a script written
  // against the manual keeps working after the rebind: no argument is a
  // float in [0, 1), one argument is [1, m], two are [m, n].
  if (arguments <= 0) {
    lua_pushnumber(state, static_cast<lua_Number>(
                              runtime_binding().services->random_double(
                                  runtime_binding().world)));
    return 1;
  }
  if (arguments == 1) {
    const lua_Integer upper = luaL_checkinteger(state, 1);
    if (upper < 1) {
      return luaL_error(state, "engine.random(m) needs m >= 1");
    }
    lua_pushinteger(state, draw_range(1, upper));
    return 1;
  }
  const lua_Integer lower = luaL_checkinteger(state, 1);
  const lua_Integer upper = luaL_checkinteger(state, 2);
  if (upper < lower) {
    return luaL_error(state, "engine.random(m, n) needs n >= m");
  }
  lua_pushinteger(state, draw_range(lower, upper));
  return 1;
}

int lua_engine_random_int(lua_State *state) noexcept {
  if (!random_available()) {
    return luaL_error(state, "engine.random_int needs a bound runtime");
  }
  const lua_Integer lower = luaL_checkinteger(state, 1);
  const lua_Integer upper = luaL_checkinteger(state, 2);
  if (upper < lower) {
    return luaL_error(state, "engine.random_int(m, n) needs n >= m");
  }
  lua_pushinteger(state, draw_range(lower, upper));
  return 1;
}

int lua_engine_set_seed(lua_State *state) noexcept {
  if (!random_available()) {
    return luaL_error(state, "engine.set_seed needs a bound runtime");
  }
  // Taken as an integer and reinterpreted, so every 64-bit value is
  // reachable from Lua including the negative half.
  const lua_Integer seed = luaL_checkinteger(state, 1);
  runtime_binding().services->seed_random(
      runtime_binding().world, static_cast<std::uint64_t>(seed));
  return 0;
}

void register_random_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_random);
  lua_setfield(state, -2, "random");
  lua_pushcfunction(state, &lua_engine_random_int);
  lua_setfield(state, -2, "random_int");
  lua_pushcfunction(state, &lua_engine_set_seed);
  lua_setfield(state, -2, "set_seed");
}

void install_engine_random_over_math(lua_State *state) noexcept {
  if (state == nullptr) {
    return;
  }
  lua_getglobal(state, "math");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  // math.random keeps its documented arities through the same function.
  lua_pushcfunction(state, &lua_engine_random);
  lua_setfield(state, -2, "random");
  // math.randomseed's Lua 5.4 no-argument form reseeds from entropy,
  // which is the behaviour this rebind exists to remove, so the alias
  // requires the seed a caller means.
  lua_pushcfunction(state, &lua_engine_set_seed);
  lua_setfield(state, -2, "randomseed");
  lua_pop(state, 1);
}

} // namespace engine::scripting
