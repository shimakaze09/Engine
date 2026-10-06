// Implements install_interpolation_math: one C function per entry, taking
// its Lua numbers as floats, calling engine/math/interpolation.h and
// pushing the result. math.clamp refuses bounds out of order rather than
// guessing which was meant, and math.smooth_damp returns the new velocity
// beside the value, since a Lua number cannot be updated in place.

#include "interpolation_math_library.h"

#include <iterator>
#include <limits>

#include "engine/math/interpolation.h"
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

int math_lerp(lua_State *state) noexcept {
  return push(state, math::lerp(argument(state, 1), argument(state, 2),
                                argument(state, 3)));
}

int math_inverse_lerp(lua_State *state) noexcept {
  return push(state, math::inverse_lerp(argument(state, 1), argument(state, 2),
                                        argument(state, 3)));
}

int math_remap(lua_State *state) noexcept {
  return push(state, math::remap(argument(state, 1), argument(state, 2),
                                 argument(state, 3), argument(state, 4),
                                 argument(state, 5)));
}

/// math.clamp(value, lo, hi); an error when lo > hi.
int math_clamp(lua_State *state) noexcept {
  const float lo = argument(state, 2);
  const float hi = argument(state, 3);
  if (lo > hi) {
    return luaL_error(state, "math.clamp: lo (%f) is greater than hi (%f)",
                      static_cast<lua_Number>(lo),
                      static_cast<lua_Number>(hi));
  }
  return push(state, math::clamp(argument(state, 1), lo, hi));
}

int math_saturate(lua_State *state) noexcept {
  return push(state, math::saturate(argument(state, 1)));
}

int math_smoothstep(lua_State *state) noexcept {
  return push(state, math::smoothstep(argument(state, 1), argument(state, 2),
                                      argument(state, 3)));
}

int math_move_towards(lua_State *state) noexcept {
  return push(state, math::move_towards(argument(state, 1), argument(state, 2),
                                        argument(state, 3)));
}

int math_move_towards_angle(lua_State *state) noexcept {
  return push(state,
              math::move_towards_angle(argument(state, 1), argument(state, 2),
                                       argument(state, 3)));
}

int math_exp_decay(lua_State *state) noexcept {
  return push(state, math::exp_decay(argument(state, 1), argument(state, 2),
                                     argument(state, 3), argument(state, 4)));
}

/// math.smooth_damp(current, target, velocity, smooth_time, dt
/// [, max_speed]) -> value, velocity.
int math_smooth_damp(lua_State *state) noexcept {
  float velocity = argument(state, 3);
  const float maxSpeed = static_cast<float>(
      luaL_optnumber(state, 6, std::numeric_limits<lua_Number>::infinity()));
  const float value =
      math::smooth_damp(argument(state, 1), argument(state, 2), &velocity,
                        argument(state, 4), maxSpeed, argument(state, 5));
  push(state, value);
  push(state, velocity);
  return 2;
}

int math_wrap_angle(lua_State *state) noexcept {
  return push(state, math::wrap_angle(argument(state, 1)));
}

int math_delta_angle(lua_State *state) noexcept {
  return push(state, math::delta_angle(argument(state, 1), argument(state, 2)));
}

int math_lerp_angle(lua_State *state) noexcept {
  return push(state, math::lerp_angle(argument(state, 1), argument(state, 2),
                                      argument(state, 3)));
}

/// math.wrap(value, length): value wrapped into [0, length). Named for
/// what it does, since `repeat` is a Lua keyword.
int math_wrap(lua_State *state) noexcept {
  return push(state, math::repeat(argument(state, 1), argument(state, 2)));
}

int math_ping_pong(lua_State *state) noexcept {
  return push(state, math::ping_pong(argument(state, 1), argument(state, 2)));
}

struct Entry final {
  const char *name;
  lua_CFunction function;
};

constexpr Entry kEntries[] = {
    {"lerp", &math_lerp},
    {"inverse_lerp", &math_inverse_lerp},
    {"remap", &math_remap},
    {"clamp", &math_clamp},
    {"saturate", &math_saturate},
    {"smoothstep", &math_smoothstep},
    {"move_towards", &math_move_towards},
    {"move_towards_angle", &math_move_towards_angle},
    {"exp_decay", &math_exp_decay},
    {"smooth_damp", &math_smooth_damp},
    {"wrap_angle", &math_wrap_angle},
    {"delta_angle", &math_delta_angle},
    {"lerp_angle", &math_lerp_angle},
    {"wrap", &math_wrap},
    {"ping_pong", &math_ping_pong},
};

} // namespace

void install_interpolation_math(lua_State *state) noexcept {
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
