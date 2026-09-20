// Implements install_deterministic_math: one C function per rebound
// entry, each taking its Lua number as a float, calling the scalar set
// and pushing the result. atan keeps Lua 5.4's two-argument form (the
// second argument defaults to 1) and log keeps its optional base, taken
// as the quotient of two deterministic logs.

#include "deterministic_math_library.h"

#include "engine/math/scalar.h"
#include "lauxlib.h"
#include "lua.h"

namespace engine::scripting {

namespace {

float argument(lua_State *state, int index) noexcept {
  return static_cast<float>(luaL_checknumber(state, index));
}

int push(lua_State *state, float value) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(value));
  return 1;
}

int det_math_sin(lua_State *state) noexcept {
  return push(state, math::det_sin(argument(state, 1)));
}

int det_math_cos(lua_State *state) noexcept {
  return push(state, math::det_cos(argument(state, 1)));
}

int det_math_tan(lua_State *state) noexcept {
  return push(state, math::det_tan(argument(state, 1)));
}

int det_math_asin(lua_State *state) noexcept {
  return push(state, math::det_asin(argument(state, 1)));
}

int det_math_acos(lua_State *state) noexcept {
  return push(state, math::det_acos(argument(state, 1)));
}

/// math.atan(y [, x]): the two-argument form Lua 5.4 documents, x
/// defaulting to 1 so the one-argument form is the plain arctangent.
int det_math_atan(lua_State *state) noexcept {
  const float y = argument(state, 1);
  const float x = static_cast<float>(luaL_optnumber(state, 2, 1.0));
  return push(state, math::det_atan2(y, x));
}

int det_math_exp(lua_State *state) noexcept {
  return push(state, math::det_exp(argument(state, 1)));
}

/// math.log(x [, base]).
int det_math_log(lua_State *state) noexcept {
  const float x = argument(state, 1);
  if (lua_isnoneornil(state, 2)) {
    return push(state, math::det_log(x));
  }
  const float base = argument(state, 2);
  return push(state, math::det_log(x) / math::det_log(base));
}

struct Entry final {
  const char *name;
  lua_CFunction function;
};

constexpr Entry kEntries[] = {
    {"sin", &det_math_sin},   {"cos", &det_math_cos},
    {"tan", &det_math_tan},   {"asin", &det_math_asin},
    {"acos", &det_math_acos}, {"atan", &det_math_atan},
    {"exp", &det_math_exp},   {"log", &det_math_log},
};

} // namespace

void install_deterministic_math(lua_State *state) noexcept {
  if (state == nullptr) {
    return;
  }
  lua_getglobal(state, "math");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  for (const Entry &entry : kEntries) {
    lua_pushcfunction(state, entry.function);
    lua_setfield(state, -2, entry.name);
  }
  lua_pop(state, 1);
}

} // namespace engine::scripting
