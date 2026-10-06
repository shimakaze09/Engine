// Implements the Lua vec2, vec3 and quat values. Each type has a metatable
// (named engine.vec2, engine.vec3 and engine.quat in the registry) whose
// __index reads the components and otherwise looks the key up in the
// type's global table, which holds its methods, its constants and its
// static functions alike: a:dot(b) and vec3.dot(a, b) are the same call.
// Values never change once built, so a constant such as vec3.up can be
// shared and hot reload has nothing to roll back in them.

#include "lua_math_values.h"

#include <cstddef>
#include <iterator>
#include <limits>

#include "engine/math/interpolation.h"
#include "lauxlib.h"
#include "lua.h"

namespace engine::scripting {

namespace {

constexpr const char *kVec2Type = "engine.vec2";
constexpr const char *kVec3Type = "engine.vec3";
constexpr const char *kQuatType = "engine.quat";

// Lua aligns userdata for its own numbers only, and math::Quat asks for 16
// bytes, so values are stored as plain floats and copied out to operate.
struct Vec2Data final {
  float x;
  float y;
};
struct Vec3Data final {
  float x;
  float y;
  float z;
};
struct QuatData final {
  float x;
  float y;
  float z;
  float w;
};

float check_float(lua_State *state, int index) noexcept {
  return static_cast<float>(luaL_checknumber(state, index));
}

int push_float(lua_State *state, float value) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(value));
  return 1;
}

math::Vec2 check_vec2(lua_State *state, int index) noexcept {
  const auto *data =
      static_cast<const Vec2Data *>(luaL_checkudata(state, index, kVec2Type));
  return math::Vec2(data->x, data->y);
}

math::Vec3 check_vec3(lua_State *state, int index) noexcept {
  const auto *data =
      static_cast<const Vec3Data *>(luaL_checkudata(state, index, kVec3Type));
  return math::Vec3(data->x, data->y, data->z);
}

math::Quat check_quat(lua_State *state, int index) noexcept {
  const auto *data =
      static_cast<const QuatData *>(luaL_checkudata(state, index, kQuatType));
  return math::Quat(data->x, data->y, data->z, data->w);
}

/// The single-letter component a string key names, or 0.
char component_key(lua_State *state, int index) noexcept {
  if (lua_type(state, index) != LUA_TSTRING) {
    return 0;
  }
  std::size_t length = 0U;
  const char *key = lua_tolstring(state, index, &length);
  return (length == 1U) ? key[0] : 0;
}

/// __index's fallback: the key in the type's global table (upvalue 1).
int index_type_table(lua_State *state) noexcept {
  lua_pushvalue(state, 2);
  lua_rawget(state, lua_upvalueindex(1));
  return 1;
}

int refuse_change(lua_State *state) noexcept {
  const char *name = (luaL_getmetafield(state, 1, "__name") == LUA_TSTRING)
                         ? lua_tostring(state, -1)
                         : "math";
  return luaL_error(state,
                    "%s values cannot be changed; build a new one instead",
                    name);
}

// --- vec2 --------------------------------------------------------------

int vec2_index(lua_State *state) noexcept {
  const math::Vec2 v = check_vec2(state, 1);
  switch (component_key(state, 2)) {
  case 'x':
    return push_float(state, v.x);
  case 'y':
    return push_float(state, v.y);
  default:
    return index_type_table(state);
  }
}

int vec2_add(lua_State *state) noexcept {
  push_vec2_value(state,
                  math::add(check_vec2(state, 1), check_vec2(state, 2)));
  return 1;
}

int vec2_sub(lua_State *state) noexcept {
  push_vec2_value(state,
                  math::sub(check_vec2(state, 1), check_vec2(state, 2)));
  return 1;
}

int vec2_mul(lua_State *state) noexcept {
  if (lua_type(state, 1) == LUA_TNUMBER) {
    push_vec2_value(state, math::mul(check_vec2(state, 2), check_float(state, 1)));
  } else {
    push_vec2_value(state, math::mul(check_vec2(state, 1), check_float(state, 2)));
  }
  return 1;
}

int vec2_div(lua_State *state) noexcept {
  push_vec2_value(state, math::div(check_vec2(state, 1), check_float(state, 2)));
  return 1;
}

int vec2_unm(lua_State *state) noexcept {
  push_vec2_value(state, math::mul(check_vec2(state, 1), -1.0F));
  return 1;
}

/// == between two values of different types is false, never an error.
int vec2_eq(lua_State *state) noexcept {
  const auto *a =
      static_cast<const Vec2Data *>(luaL_testudata(state, 1, kVec2Type));
  const auto *b =
      static_cast<const Vec2Data *>(luaL_testudata(state, 2, kVec2Type));
  lua_pushboolean(state, (a != nullptr) && (b != nullptr) && (a->x == b->x) &&
                             (a->y == b->y));
  return 1;
}

int vec2_tostring(lua_State *state) noexcept {
  const math::Vec2 v = check_vec2(state, 1);
  lua_pushfstring(state, "vec2(%f, %f)", static_cast<lua_Number>(v.x),
                  static_cast<lua_Number>(v.y));
  return 1;
}

int vec2_length(lua_State *state) noexcept {
  return push_float(state, math::length(check_vec2(state, 1)));
}

int vec2_length_sq(lua_State *state) noexcept {
  return push_float(state, math::length_sq(check_vec2(state, 1)));
}

int vec2_normalized(lua_State *state) noexcept {
  push_vec2_value(state, math::normalize(check_vec2(state, 1)));
  return 1;
}

int vec2_dot(lua_State *state) noexcept {
  return push_float(state,
                    math::dot(check_vec2(state, 1), check_vec2(state, 2)));
}

int vec2_distance(lua_State *state) noexcept {
  return push_float(state, math::length(math::sub(check_vec2(state, 1),
                                                  check_vec2(state, 2))));
}

int vec2_lerp(lua_State *state) noexcept {
  push_vec2_value(state, math::lerp(check_vec2(state, 1), check_vec2(state, 2),
                                    check_float(state, 3)));
  return 1;
}

int vec2_unpack(lua_State *state) noexcept {
  const math::Vec2 v = check_vec2(state, 1);
  push_float(state, v.x);
  push_float(state, v.y);
  return 2;
}

/// vec2(x, y), or vec2() for zero.
int vec2_new(lua_State *state) noexcept {
  if (lua_gettop(state) <= 1) {
    push_vec2_value(state, math::Vec2(0.0F, 0.0F));
    return 1;
  }
  push_vec2_value(state, math::Vec2(check_float(state, 2), check_float(state, 3)));
  return 1;
}

// --- vec3 --------------------------------------------------------------

int vec3_index(lua_State *state) noexcept {
  const math::Vec3 v = check_vec3(state, 1);
  switch (component_key(state, 2)) {
  case 'x':
    return push_float(state, v.x);
  case 'y':
    return push_float(state, v.y);
  case 'z':
    return push_float(state, v.z);
  default:
    return index_type_table(state);
  }
}

int vec3_add(lua_State *state) noexcept {
  push_vec3_value(state,
                  math::add(check_vec3(state, 1), check_vec3(state, 2)));
  return 1;
}

int vec3_sub(lua_State *state) noexcept {
  push_vec3_value(state,
                  math::sub(check_vec3(state, 1), check_vec3(state, 2)));
  return 1;
}

int vec3_mul(lua_State *state) noexcept {
  if (lua_type(state, 1) == LUA_TNUMBER) {
    push_vec3_value(state, math::mul(check_vec3(state, 2), check_float(state, 1)));
  } else {
    push_vec3_value(state, math::mul(check_vec3(state, 1), check_float(state, 2)));
  }
  return 1;
}

int vec3_div(lua_State *state) noexcept {
  push_vec3_value(state, math::div(check_vec3(state, 1), check_float(state, 2)));
  return 1;
}

int vec3_unm(lua_State *state) noexcept {
  push_vec3_value(state, math::negate(check_vec3(state, 1)));
  return 1;
}

int vec3_eq(lua_State *state) noexcept {
  math::Vec3 a{};
  math::Vec3 b{};
  lua_pushboolean(state, to_vec3_value(state, 1, &a) &&
                             to_vec3_value(state, 2, &b) && (a.x == b.x) &&
                             (a.y == b.y) && (a.z == b.z));
  return 1;
}

int vec3_tostring(lua_State *state) noexcept {
  const math::Vec3 v = check_vec3(state, 1);
  lua_pushfstring(state, "vec3(%f, %f, %f)", static_cast<lua_Number>(v.x),
                  static_cast<lua_Number>(v.y), static_cast<lua_Number>(v.z));
  return 1;
}

int vec3_length(lua_State *state) noexcept {
  return push_float(state, math::length(check_vec3(state, 1)));
}

int vec3_length_sq(lua_State *state) noexcept {
  return push_float(state, math::length_sq(check_vec3(state, 1)));
}

int vec3_normalized(lua_State *state) noexcept {
  push_vec3_value(state, math::normalize(check_vec3(state, 1)));
  return 1;
}

int vec3_dot(lua_State *state) noexcept {
  return push_float(state,
                    math::dot(check_vec3(state, 1), check_vec3(state, 2)));
}

int vec3_cross(lua_State *state) noexcept {
  push_vec3_value(state,
                  math::cross(check_vec3(state, 1), check_vec3(state, 2)));
  return 1;
}

int vec3_distance(lua_State *state) noexcept {
  return push_float(state,
                    math::distance(check_vec3(state, 1), check_vec3(state, 2)));
}

int vec3_lerp(lua_State *state) noexcept {
  push_vec3_value(state, math::lerp(check_vec3(state, 1), check_vec3(state, 2),
                                    check_float(state, 3)));
  return 1;
}

int vec3_move_towards(lua_State *state) noexcept {
  push_vec3_value(state,
                  math::move_towards(check_vec3(state, 1), check_vec3(state, 2),
                                     check_float(state, 3)));
  return 1;
}

int vec3_exp_decay(lua_State *state) noexcept {
  push_vec3_value(state,
                  math::exp_decay(check_vec3(state, 1), check_vec3(state, 2),
                                  check_float(state, 3), check_float(state, 4)));
  return 1;
}

/// vec3.smooth_damp(current, target, velocity, smooth_time, dt
/// [, max_speed]) -> value, velocity.
int vec3_smooth_damp(lua_State *state) noexcept {
  math::Vec3 velocity = check_vec3(state, 3);
  const float maxSpeed = static_cast<float>(
      luaL_optnumber(state, 6, std::numeric_limits<lua_Number>::infinity()));
  const math::Vec3 value =
      math::smooth_damp(check_vec3(state, 1), check_vec3(state, 2), &velocity,
                        check_float(state, 4), maxSpeed, check_float(state, 5));
  push_vec3_value(state, value);
  push_vec3_value(state, velocity);
  return 2;
}

int vec3_unpack(lua_State *state) noexcept {
  const math::Vec3 v = check_vec3(state, 1);
  push_float(state, v.x);
  push_float(state, v.y);
  push_float(state, v.z);
  return 3;
}

/// vec3(x, y, z), or vec3() for zero.
int vec3_new(lua_State *state) noexcept {
  if (lua_gettop(state) <= 1) {
    push_vec3_value(state, math::Vec3(0.0F, 0.0F, 0.0F));
    return 1;
  }
  push_vec3_value(state, math::Vec3(check_float(state, 2), check_float(state, 3),
                                    check_float(state, 4)));
  return 1;
}

// --- quat --------------------------------------------------------------

int quat_index(lua_State *state) noexcept {
  const math::Quat q = check_quat(state, 1);
  switch (component_key(state, 2)) {
  case 'x':
    return push_float(state, q.x);
  case 'y':
    return push_float(state, q.y);
  case 'z':
    return push_float(state, q.z);
  case 'w':
    return push_float(state, q.w);
  default:
    return index_type_table(state);
  }
}

/// quat * quat composes (the right-hand rotation first); quat * vec3
/// rotates the vector.
int quat_mul(lua_State *state) noexcept {
  const math::Quat q = check_quat(state, 1);
  math::Vec3 v{};
  if (to_vec3_value(state, 2, &v)) {
    push_vec3_value(state, math::rotate_vector(v, q));
    return 1;
  }
  push_quat_value(state, math::mul(q, check_quat(state, 2)));
  return 1;
}

int quat_eq(lua_State *state) noexcept {
  math::Quat a{};
  math::Quat b{};
  lua_pushboolean(state, to_quat_value(state, 1, &a) &&
                             to_quat_value(state, 2, &b) && (a.x == b.x) &&
                             (a.y == b.y) && (a.z == b.z) && (a.w == b.w));
  return 1;
}

int quat_tostring(lua_State *state) noexcept {
  const math::Quat q = check_quat(state, 1);
  lua_pushfstring(state, "quat(%f, %f, %f, %f)", static_cast<lua_Number>(q.x),
                  static_cast<lua_Number>(q.y), static_cast<lua_Number>(q.z),
                  static_cast<lua_Number>(q.w));
  return 1;
}

int quat_inverse(lua_State *state) noexcept {
  push_quat_value(state, math::inverse(check_quat(state, 1)));
  return 1;
}

int quat_normalized(lua_State *state) noexcept {
  push_quat_value(state, math::normalize(check_quat(state, 1)));
  return 1;
}

int quat_angle(lua_State *state) noexcept {
  return push_float(state,
                    math::angle(check_quat(state, 1), check_quat(state, 2)));
}

int quat_forward(lua_State *state) noexcept {
  push_vec3_value(state, math::forward(check_quat(state, 1)));
  return 1;
}

int quat_back(lua_State *state) noexcept {
  push_vec3_value(state, math::back(check_quat(state, 1)));
  return 1;
}

int quat_right(lua_State *state) noexcept {
  push_vec3_value(state, math::right(check_quat(state, 1)));
  return 1;
}

int quat_up(lua_State *state) noexcept {
  push_vec3_value(state, math::up(check_quat(state, 1)));
  return 1;
}

/// The order named by the optional string at `index`, YXZ by default.
math::EulerOrder check_order(lua_State *state, int index) noexcept {
  static const char *const kOrders[] = {"XYZ", "XZY", "YXZ",
                                        "YZX", "ZXY", "ZYX", nullptr};
  return static_cast<math::EulerOrder>(
      luaL_checkoption(state, index, "YXZ", kOrders));
}

/// q:euler([order]) -> pitch, yaw, roll in radians.
int quat_to_euler(lua_State *state) noexcept {
  const math::Quat q = check_quat(state, 1);
  float pitch = 0.0F;
  float yaw = 0.0F;
  float roll = 0.0F;
  static_cast<void>(
      math::to_euler(q, check_order(state, 2), &pitch, &yaw, &roll));
  push_float(state, pitch);
  push_float(state, yaw);
  push_float(state, roll);
  return 3;
}

int quat_unpack(lua_State *state) noexcept {
  const math::Quat q = check_quat(state, 1);
  push_float(state, q.x);
  push_float(state, q.y);
  push_float(state, q.z);
  push_float(state, q.w);
  return 4;
}

/// quat.euler(pitch, yaw, roll [, order]), radians.
int quat_from_euler(lua_State *state) noexcept {
  push_quat_value(state, math::from_euler(check_float(state, 1),
                                          check_float(state, 2),
                                          check_float(state, 3),
                                          check_order(state, 4)));
  return 1;
}

/// quat.angle_axis(radians, axis).
int quat_angle_axis(lua_State *state) noexcept {
  push_quat_value(state,
                  math::from_axis_angle(check_vec3(state, 2), check_float(state, 1)));
  return 1;
}

int quat_from_to(lua_State *state) noexcept {
  push_quat_value(state,
                  math::from_to(check_vec3(state, 1), check_vec3(state, 2)));
  return 1;
}

/// quat.look_rotation(forward [, up]) -> quat, or nil when forward is zero
/// or parallel to up (up defaults to +Y).
int quat_look_rotation(lua_State *state) noexcept {
  const math::Vec3 forward = check_vec3(state, 1);
  math::Vec3 up(0.0F, 1.0F, 0.0F);
  if (!lua_isnoneornil(state, 2)) {
    up = check_vec3(state, 2);
  }
  math::Quat out{};
  if (!math::look_rotation(forward, up, &out)) {
    lua_pushnil(state);
    return 1;
  }
  push_quat_value(state, out);
  return 1;
}

int quat_slerp(lua_State *state) noexcept {
  push_quat_value(state, math::slerp(check_quat(state, 1), check_quat(state, 2),
                                     check_float(state, 3)));
  return 1;
}

int quat_nlerp(lua_State *state) noexcept {
  push_quat_value(state, math::nlerp(check_quat(state, 1), check_quat(state, 2),
                                     check_float(state, 3)));
  return 1;
}

int quat_rotate_towards(lua_State *state) noexcept {
  push_quat_value(state,
                  math::rotate_towards(check_quat(state, 1), check_quat(state, 2),
                                       check_float(state, 3)));
  return 1;
}

/// quat(x, y, z, w), or quat() for identity.
int quat_new(lua_State *state) noexcept {
  if (lua_gettop(state) <= 1) {
    push_quat_value(state, math::Quat());
    return 1;
  }
  push_quat_value(state, math::Quat(check_float(state, 2), check_float(state, 3),
                                    check_float(state, 4), check_float(state, 5)));
  return 1;
}

// --- registration ------------------------------------------------------

struct Function final {
  const char *name;
  lua_CFunction function;
};

/// Builds one type: its metatable under `typeName` with `metamethods` and
/// an __index over the global table `globalName`, which holds `functions`,
/// is callable through `constructor`, and gets its constants from the
/// caller once this returns with it on the stack top.
void install_type(lua_State *state, const char *typeName,
                  const char *globalName, lua_CFunction index,
                  const Function *metamethods, std::size_t metamethodCount,
                  const Function *functions, std::size_t functionCount,
                  lua_CFunction constructor) noexcept {
  lua_newtable(state); // the global table
  for (std::size_t i = 0U; i < functionCount; ++i) {
    lua_pushcfunction(state, functions[i].function);
    lua_setfield(state, -2, functions[i].name);
  }
  lua_newtable(state); // its metatable: calling it constructs
  lua_pushcfunction(state, constructor);
  lua_setfield(state, -2, "__call");
  lua_setmetatable(state, -2);

  luaL_newmetatable(state, typeName);
  for (std::size_t i = 0U; i < metamethodCount; ++i) {
    lua_pushcfunction(state, metamethods[i].function);
    lua_setfield(state, -2, metamethods[i].name);
  }
  lua_pushcfunction(state, &refuse_change);
  lua_setfield(state, -2, "__newindex");
  lua_pushstring(state, globalName);
  lua_setfield(state, -2, "__name");
  lua_pushvalue(state, -2);
  lua_pushcclosure(state, index, 1);
  lua_setfield(state, -2, "__index");
  lua_pop(state, 1);

  lua_pushvalue(state, -1);
  lua_setglobal(state, globalName);
}

void set_vec2(lua_State *state, const char *name, float x, float y) noexcept {
  push_vec2_value(state, math::Vec2(x, y));
  lua_setfield(state, -2, name);
}

void set_vec3(lua_State *state, const char *name, float x, float y,
              float z) noexcept {
  push_vec3_value(state, math::Vec3(x, y, z));
  lua_setfield(state, -2, name);
}

} // namespace

void push_vec2_value(lua_State *state, const math::Vec2 &value) noexcept {
  auto *data =
      static_cast<Vec2Data *>(lua_newuserdatauv(state, sizeof(Vec2Data), 0));
  data->x = value.x;
  data->y = value.y;
  luaL_setmetatable(state, kVec2Type);
}

void push_vec3_value(lua_State *state, const math::Vec3 &value) noexcept {
  auto *data =
      static_cast<Vec3Data *>(lua_newuserdatauv(state, sizeof(Vec3Data), 0));
  data->x = value.x;
  data->y = value.y;
  data->z = value.z;
  luaL_setmetatable(state, kVec3Type);
}

void push_quat_value(lua_State *state, const math::Quat &value) noexcept {
  auto *data =
      static_cast<QuatData *>(lua_newuserdatauv(state, sizeof(QuatData), 0));
  data->x = value.x;
  data->y = value.y;
  data->z = value.z;
  data->w = value.w;
  luaL_setmetatable(state, kQuatType);
}

bool to_vec3_value(lua_State *state, int index, math::Vec3 *out) noexcept {
  const auto *data =
      static_cast<const Vec3Data *>(luaL_testudata(state, index, kVec3Type));
  if ((data == nullptr) || (out == nullptr)) {
    return false;
  }
  *out = math::Vec3(data->x, data->y, data->z);
  return true;
}

bool to_quat_value(lua_State *state, int index, math::Quat *out) noexcept {
  const auto *data =
      static_cast<const QuatData *>(luaL_testudata(state, index, kQuatType));
  if ((data == nullptr) || (out == nullptr)) {
    return false;
  }
  *out = math::Quat(data->x, data->y, data->z, data->w);
  return true;
}

void install_math_values(lua_State *state) noexcept {
  if (state == nullptr) {
    return;
  }

  static constexpr Function kVec2Meta[] = {
      {"__add", &vec2_add}, {"__sub", &vec2_sub},
      {"__mul", &vec2_mul}, {"__div", &vec2_div},
      {"__unm", &vec2_unm}, {"__eq", &vec2_eq},
      {"__tostring", &vec2_tostring}};
  static constexpr Function kVec2Functions[] = {
      {"length", &vec2_length},     {"length_sq", &vec2_length_sq},
      {"normalized", &vec2_normalized}, {"dot", &vec2_dot},
      {"distance", &vec2_distance}, {"lerp", &vec2_lerp},
      {"unpack", &vec2_unpack}};
  install_type(state, kVec2Type, "vec2", &vec2_index, kVec2Meta,
               std::size(kVec2Meta), kVec2Functions, std::size(kVec2Functions),
               &vec2_new);
  set_vec2(state, "zero", 0.0F, 0.0F);
  set_vec2(state, "one", 1.0F, 1.0F);
  set_vec2(state, "up", 0.0F, 1.0F);
  set_vec2(state, "down", 0.0F, -1.0F);
  set_vec2(state, "right", 1.0F, 0.0F);
  set_vec2(state, "left", -1.0F, 0.0F);
  lua_pop(state, 1);

  static constexpr Function kVec3Meta[] = {
      {"__add", &vec3_add}, {"__sub", &vec3_sub},
      {"__mul", &vec3_mul}, {"__div", &vec3_div},
      {"__unm", &vec3_unm}, {"__eq", &vec3_eq},
      {"__tostring", &vec3_tostring}};
  static constexpr Function kVec3Functions[] = {
      {"length", &vec3_length},
      {"length_sq", &vec3_length_sq},
      {"normalized", &vec3_normalized},
      {"dot", &vec3_dot},
      {"cross", &vec3_cross},
      {"distance", &vec3_distance},
      {"lerp", &vec3_lerp},
      {"move_towards", &vec3_move_towards},
      {"exp_decay", &vec3_exp_decay},
      {"smooth_damp", &vec3_smooth_damp},
      {"unpack", &vec3_unpack}};
  install_type(state, kVec3Type, "vec3", &vec3_index, kVec3Meta,
               std::size(kVec3Meta), kVec3Functions, std::size(kVec3Functions),
               &vec3_new);
  set_vec3(state, "zero", 0.0F, 0.0F, 0.0F);
  set_vec3(state, "one", 1.0F, 1.0F, 1.0F);
  set_vec3(state, "up", 0.0F, 1.0F, 0.0F);
  set_vec3(state, "down", 0.0F, -1.0F, 0.0F);
  set_vec3(state, "right", 1.0F, 0.0F, 0.0F);
  set_vec3(state, "left", -1.0F, 0.0F, 0.0F);
  // Forward is -Z, the engine's camera and look_rotation convention.
  set_vec3(state, "forward", 0.0F, 0.0F, -1.0F);
  set_vec3(state, "back", 0.0F, 0.0F, 1.0F);
  lua_pop(state, 1);

  static constexpr Function kQuatMeta[] = {{"__mul", &quat_mul},
                                           {"__eq", &quat_eq},
                                           {"__tostring", &quat_tostring}};
  static constexpr Function kQuatFunctions[] = {
      {"inverse", &quat_inverse},
      {"normalized", &quat_normalized},
      {"angle", &quat_angle},
      {"forward", &quat_forward},
      {"back", &quat_back},
      {"right", &quat_right},
      {"up", &quat_up},
      {"to_euler", &quat_to_euler},
      {"unpack", &quat_unpack},
      {"euler", &quat_from_euler},
      {"angle_axis", &quat_angle_axis},
      {"from_to", &quat_from_to},
      {"look_rotation", &quat_look_rotation},
      {"slerp", &quat_slerp},
      {"nlerp", &quat_nlerp},
      {"rotate_towards", &quat_rotate_towards}};
  install_type(state, kQuatType, "quat", &quat_index, kQuatMeta,
               std::size(kQuatMeta), kQuatFunctions, std::size(kQuatFunctions),
               &quat_new);
  push_quat_value(state, math::Quat());
  lua_setfield(state, -2, "identity");
  lua_pop(state, 1);
}

} // namespace engine::scripting
