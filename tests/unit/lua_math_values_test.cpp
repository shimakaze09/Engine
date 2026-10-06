// Tests the Lua vector and rotation values and the interpolation functions
// through the production VM. A script evaluates each function on fixed
// inputs and logs the bits of the float it got; the test compares them
// with the same call into engine/math, so a script blends, turns and moves
// exactly as engine code does. The script also checks the values' rules
// (immutable, typed equality, constructors, constants, refused bounds),
// and, against a World, that every vector-taking binding accepts a value
// as well as the loose numbers it always took, and refuses the wrong one.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/math/interpolation.h"
#include "engine/math/quat.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace math = engine::math;
namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "lua_math_values_test.lua";
constexpr int kMaxReports = 128;

/// One "LUAMATH <name> <bits>" line the script logged.
struct Report final {
  char name[40] = {};
  std::uint32_t bits = 0U;
};

Report g_reports[kMaxReports]{};
int g_reportCount = 0;

void capture(engine::core::LogLevel, const char *channel, const char *message,
             void *) noexcept {
  if ((channel == nullptr) || (std::strcmp(channel, "scripting") != 0) ||
      (message == nullptr) || (std::strncmp(message, "LUAMATH ", 8U) != 0) ||
      (g_reportCount >= kMaxReports)) {
    return;
  }
  const char *cursor = message + 8;
  const char *space = std::strchr(cursor, ' ');
  Report report{};
  if ((space == nullptr) || (space == cursor) ||
      (static_cast<std::size_t>(space - cursor) >= sizeof(report.name))) {
    return;
  }
  std::memcpy(report.name, cursor, static_cast<std::size_t>(space - cursor));
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(space + 1, &end, 16);
  if ((end == (space + 1)) || (*end != '\0')) {
    return;
  }
  report.bits = static_cast<std::uint32_t>(parsed);
  g_reports[g_reportCount] = report;
  ++g_reportCount;
}

bool write_script(const char *code) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(code);
  const bool ok = (std::fwrite(code, 1U, length, file) == length);
  std::fclose(file);
  return ok;
}

std::uint32_t bits_of(float value) noexcept {
  std::uint32_t out = 0U;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

/// True when the script reported `name` with exactly the bits of `value`.
bool reported_as(const char *name, float value) noexcept {
  for (int i = 0; i < g_reportCount; ++i) {
    if (std::strcmp(g_reports[i].name, name) == 0) {
      if (g_reports[i].bits != bits_of(value)) {
        std::printf("  %s: script %08X engine %08X\n", name,
                    g_reports[i].bits, bits_of(value));
        return false;
      }
      return true;
    }
  }
  std::printf("  %s: never reported\n", name);
  return false;
}

bool reported_as(const char *name, const math::Vec3 &v) noexcept {
  char key[40] = {};
  bool ok = true;
  const float parts[3] = {v.x, v.y, v.z};
  const char suffix[3] = {'x', 'y', 'z'};
  for (int i = 0; i < 3; ++i) {
    std::snprintf(key, sizeof(key), "%s.%c", name, suffix[i]);
    ok = reported_as(key, parts[i]) && ok;
  }
  return ok;
}

bool reported_as(const char *name, const math::Quat &q) noexcept {
  char key[40] = {};
  bool ok = true;
  const float parts[4] = {q.x, q.y, q.z, q.w};
  const char suffix[4] = {'x', 'y', 'z', 'w'};
  for (int i = 0; i < 4; ++i) {
    std::snprintf(key, sizeof(key), "%s.%c", name, suffix[i]);
    ok = reported_as(key, parts[i]) && ok;
  }
  return ok;
}

constexpr const char *kScript = R"lua(
local function bits(v)
  return string.format('%08X', string.unpack('<I4', string.pack('<f', v)))
end
local function report(name, v)
  if type(v) == 'number' then
    engine.log('LUAMATH ' .. name .. ' ' .. bits(v))
  elseif getmetatable(v) == getmetatable(vec3()) then
    report(name .. '.x', v.x) report(name .. '.y', v.y) report(name .. '.z', v.z)
  else
    report(name .. '.x', v.x) report(name .. '.y', v.y)
    report(name .. '.z', v.z) report(name .. '.w', v.w)
  end
end

report('lerp', math.lerp(2.5, 7.25, 0.3))
report('inverse_lerp', math.inverse_lerp(2.5, 7.25, 4.0))
report('remap', math.remap(3.0, 1.0, 5.0, 10.0, 30.0))
report('clamp', math.clamp(1.7, -0.5, 1.25))
report('saturate', math.saturate(-0.2))
report('smoothstep', math.smoothstep(0.0, 1.0, 0.37))
report('move_towards', math.move_towards(1.0, 9.0, 2.5))
report('move_towards_angle', math.move_towards_angle(3.0, -3.0, 0.1))
report('exp_decay', math.exp_decay(1.0, 5.0, 2.0, 0.2))
local sd, sdv = math.smooth_damp(0.0, 10.0, 0.0, 0.3, 1 / 60)
report('smooth_damp', sd) report('smooth_damp_v', sdv)
report('wrap_angle', math.wrap_angle(7.5))
report('delta_angle', math.delta_angle(0.1, 6.2))
report('lerp_angle', math.lerp_angle(3.0, -3.0, 0.5))
report('wrap', math.wrap(5.5, 2.0))
report('ping_pong', math.ping_pong(3.0, 2.0))

local a = vec3(1.0, 2.0, 3.0)
local b = vec3(-0.5, 4.0, 0.25)
report('v_add', a + b) report('v_sub', a - b) report('v_mul', a * 1.5)
report('v_rmul', 1.5 * a) report('v_div', a / 3.0) report('v_unm', -a)
report('v_normalized', a:normalized()) report('v_cross', a:cross(b))
report('v_dot', a:dot(b)) report('v_length', a:length())
report('v_distance', vec3.distance(a, b)) report('v_lerp', a:lerp(b, 0.3))
report('v_move_towards', a:move_towards(b, 0.75))
report('v_exp_decay', vec3.exp_decay(a, b, 2.0, 0.2))
local vd, vdv = vec3.smooth_damp(a, b, vec3(), 0.3, 1 / 60)
report('v_smooth_damp', vd) report('v_smooth_damp_v', vdv)

local q = quat.euler(0.3, -0.5, 0.9)
report('q_euler', q)
report('q_euler_xyz', quat.euler(0.3, -0.5, 0.9, 'XYZ'))
local p, y, r = q:to_euler()
report('q_to_euler_p', p) report('q_to_euler_y', y) report('q_to_euler_r', r)
report('q_angle_axis', quat.angle_axis(1.2, vec3(1.0, 1.0, 0.0)))
report('q_from_to', quat.from_to(a, b))
report('q_rotate', q * a)
local q2 = quat.angle_axis(0.7, vec3.up)
report('q_compose', q * q2)
report('q_slerp', quat.slerp(q, q2, 0.4))
report('q_nlerp', quat.nlerp(q, q2, 0.4))
report('q_rotate_towards', quat.rotate_towards(q, q2, 0.2))
report('q_angle', q:angle(q2))
report('q_inverse', q:inverse())
report('q_forward', q:forward()) report('q_up', q:up()) report('q_right', q:right())
report('q_look', quat.look_rotation(b, vec3.up))

function check_value_rules()
  local v = vec3(1, 2, 3)
  if pcall(function() v.x = 5 end) then error('a vec3 changed') end
  if pcall(function() quat().w = 0 end) then error('a quat changed') end
  if not (vec3(1, 2, 3) == v) then error('equal vec3 values differ') end
  if vec3(1, 2, 3) == vec3(1, 2, 4) then error('different vec3 values equal') end
  if v == quat() then error('a vec3 equals a quat') end
  if vec2(1, 2) == vec2(1, 3) then error('different vec2 values equal') end
  if vec3() ~= vec3.zero or quat() ~= quat.identity then
    error('empty constructors are not zero and identity')
  end
  if vec3.forward ~= vec3(0, 0, -1) or vec3.up ~= vec3(0, 1, 0) then
    error('forward is not -Z or up is not +Y')
  end
  if tostring(v):sub(1, 5) ~= 'vec3(' then error('tostring names the type') end
  if pcall(vec3, 1, 'two', 3) then error('vec3 took a string component') end
  if pcall(math.clamp, 5, 10, 0) then error('clamp took reversed bounds') end
  if math.clamp(-1, 0, 10) ~= 0 then error('clamp below') end
  local x, y2, z = v:unpack()
  if x ~= 1 or y2 ~= 2 or z ~= 3 then error('unpack') end
  if vec3(engine.get_gravity()) == nil then error('vec3 of a binding') end
  if quat.look_rotation(vec3.zero) ~= nil then
    error('look_rotation of a zero direction is a rotation')
  end
  if vec2(3, 4):length() ~= 5 then error('vec2 length') end
end

g_wall = nil
g_mover = nil
function setup_scene()
  g_wall = engine.spawn_entity()
  engine.set_position(g_wall, vec3(6, 0, 0))
  if not engine.add_collider(g_wall, vec3(0.5, 2, 2)) then
    error('collider from a vec3 failed')
  end
  g_mover = engine.spawn_entity()
end

function check_bindings()
  if not engine.set_position(g_mover, vec3(1.5, -2, 3.25)) then
    error('set_position(vec3) refused')
  end
  if vec3(engine.get_position(g_mover)) ~= vec3(1.5, -2, 3.25) then
    error('set_position(vec3) did not land')
  end
  if not engine.set_position(g_mover, 4, 5, 6) then
    error('set_position(x, y, z) refused')
  end
  if vec3(engine.get_position(g_mover)) ~= vec3(4, 5, 6) then
    error('the loose form did not land')
  end
  if engine.set_position(g_mover, quat()) then
    error('set_position took a quat')
  end
  if engine.set_position(g_mover, vec3(0 / 0, 0, 0)) then
    error('set_position took a NaN vec3')
  end
  local turn = quat.angle_axis(math.pi / 2, vec3.up)
  if not engine.set_rotation(g_mover, turn) then
    error('set_rotation(quat) refused')
  end
  if quat(engine.get_rotation(g_mover)) ~= turn then
    error('set_rotation(quat) did not land')
  end
  if (vec3(engine.get_forward(g_mover)) - turn:forward()):length() ~= 0 then
    error('get_forward is not the rotation forward')
  end
  if (vec3(engine.get_up(g_mover)) - vec3.up):length() > 1e-6 then
    error('get_up of a turn about Y is +Y')
  end
  if engine.set_rotation(g_mover, vec3.up) then
    error('set_rotation took a vec3')
  end
  engine.set_position(g_mover, 0, 0, 0)
  if not engine.look_at(g_mover, vec3(0, 0, 5)) then
    error('look_at(vec3) refused')
  end
  if (vec3(engine.get_forward(g_mover)) - vec3.back):length() > 1e-6 then
    error('look_at(vec3) did not face the point')
  end
  engine.set_gravity(vec3(0, -3, 0))
  if vec3(engine.get_gravity()) ~= vec3(0, -3, 0) then
    error('set_gravity(vec3) did not land')
  end
  engine.set_gravity(0, -9.81, 0)
end

function check_queries()
  if engine.raycast(vec3.zero, vec3.right, 20) ~= g_wall then
    error('raycast(vec3, vec3, d) missed the wall')
  end
  if engine.raycast(0, 0, 0, vec3.right, 20) ~= g_wall then
    error('raycast mixing loose numbers and a value missed the wall')
  end
  if engine.raycast(0, 0, 0, 1, 0, 0, 20) ~= g_wall then
    error('the loose raycast missed the wall')
  end
  if engine.raycast(vec3.zero, quat(), 20) ~= nil then
    error('raycast took a quat direction')
  end
  if engine.sweep_capsule(vec3(0, -0.5, 0), vec3(0, 0.5, 0), 0.25,
                          vec3.right, 20) ~= g_wall then
    error('sweep_capsule with values missed the wall')
  end
  if engine.sweep_box(vec3.zero, vec3(0.5, 0.5, 0.5), vec3.right, 20)
      ~= g_wall then
    error('sweep_box with values missed the wall')
  end
  if engine.sweep_sphere(vec3.zero, 0.5, vec3.right, 20) ~= g_wall then
    error('sweep_sphere with values missed the wall')
  end
  local found = engine.overlap_box(vec3(6, 0, 0), vec3.one)
  if #found ~= 1 then error('overlap_box with values') end
  found = engine.overlap_sphere(vec3(6, 0, 0), 1.0)
  if #found ~= 1 then error('overlap_sphere with a value') end
end
)lua";

} // namespace

/// Runs the Lua math values suite.
int main() {
  engine::tests::TestContext ctx;
  ctx.check(engine::core::initialize_logging(), "initialize logging");
  ctx.check(engine::core::log_register_sink(&capture, nullptr),
            "register the log sink");
  ctx.check(sc::initialize_scripting(), "initialize scripting");
  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  ctx.check(world != nullptr, "a world");
  if (world == nullptr) {
    return ctx.finish("lua_math_values");
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);
  ctx.check(write_script(kScript), "write the script");
  ctx.check(sc::load_script(kScriptPath), "the script runs");

  const float inf = std::numeric_limits<float>::infinity();
  ctx.check(reported_as("lerp", math::lerp(2.5F, 7.25F, 0.3F)) &&
                reported_as("inverse_lerp",
                            math::inverse_lerp(2.5F, 7.25F, 4.0F)) &&
                reported_as("remap",
                            math::remap(3.0F, 1.0F, 5.0F, 10.0F, 30.0F)) &&
                reported_as("clamp", math::clamp(1.7F, -0.5F, 1.25F)) &&
                reported_as("saturate", math::saturate(-0.2F)) &&
                reported_as("smoothstep",
                            math::smoothstep(0.0F, 1.0F, 0.37F)) &&
                reported_as("move_towards",
                            math::move_towards(1.0F, 9.0F, 2.5F)) &&
                reported_as("move_towards_angle",
                            math::move_towards_angle(3.0F, -3.0F, 0.1F)) &&
                reported_as("exp_decay",
                            math::exp_decay(1.0F, 5.0F, 2.0F, 0.2F)),
            "the scalar blends are the engine's, bit for bit");
  float velocity = 0.0F;
  const float damped = math::smooth_damp(
      0.0F, 10.0F, &velocity, 0.3F, inf, static_cast<float>(1.0 / 60.0));
  ctx.check(reported_as("smooth_damp", damped) &&
                reported_as("smooth_damp_v", velocity),
            "math.smooth_damp returns the engine's value and velocity");
  ctx.check(reported_as("wrap_angle", math::wrap_angle(7.5F)) &&
                reported_as("delta_angle", math::delta_angle(0.1F, 6.2F)) &&
                reported_as("lerp_angle", math::lerp_angle(3.0F, -3.0F, 0.5F)) &&
                reported_as("wrap", math::repeat(5.5F, 2.0F)) &&
                reported_as("ping_pong", math::ping_pong(3.0F, 2.0F)),
            "the angle helpers are the engine's, bit for bit");

  const math::Vec3 a(1.0F, 2.0F, 3.0F);
  const math::Vec3 b(-0.5F, 4.0F, 0.25F);
  ctx.check(reported_as("v_add", math::add(a, b)) &&
                reported_as("v_sub", math::sub(a, b)) &&
                reported_as("v_mul", math::mul(a, 1.5F)) &&
                reported_as("v_rmul", math::mul(a, 1.5F)) &&
                reported_as("v_div", math::div(a, 3.0F)) &&
                reported_as("v_unm", math::negate(a)),
            "vec3 arithmetic is the engine's, bit for bit");
  ctx.check(reported_as("v_normalized", math::normalize(a)) &&
                reported_as("v_cross", math::cross(a, b)) &&
                reported_as("v_dot", math::dot(a, b)) &&
                reported_as("v_length", math::length(a)) &&
                reported_as("v_distance", math::distance(a, b)) &&
                reported_as("v_lerp", math::lerp(a, b, 0.3F)) &&
                reported_as("v_move_towards", math::move_towards(a, b, 0.75F)) &&
                reported_as("v_exp_decay", math::exp_decay(a, b, 2.0F, 0.2F)),
            "vec3 methods are the engine's, bit for bit");
  math::Vec3 pointVelocity(0.0F, 0.0F, 0.0F);
  const math::Vec3 point = math::smooth_damp(
      a, b, &pointVelocity, 0.3F, inf, static_cast<float>(1.0 / 60.0));
  ctx.check(reported_as("v_smooth_damp", point) &&
                reported_as("v_smooth_damp_v", pointVelocity),
            "vec3.smooth_damp is the engine's, bit for bit");

  const math::Quat q = math::from_euler(0.3F, -0.5F, 0.9F);
  const math::Quat q2 =
      math::from_axis_angle(math::Vec3(0.0F, 1.0F, 0.0F), 0.7F);
  float pitch = 0.0F;
  float yaw = 0.0F;
  float roll = 0.0F;
  static_cast<void>(math::to_euler(q, &pitch, &yaw, &roll));
  math::Quat look{};
  static_cast<void>(
      math::look_rotation(b, math::Vec3(0.0F, 1.0F, 0.0F), &look));
  ctx.check(reported_as("q_euler", q) &&
                reported_as("q_euler_xyz",
                            math::from_euler(0.3F, -0.5F, 0.9F,
                                             math::EulerOrder::XYZ)) &&
                reported_as("q_to_euler_p", pitch) &&
                reported_as("q_to_euler_y", yaw) &&
                reported_as("q_to_euler_r", roll),
            "Euler conversions are the engine's, bit for bit");
  ctx.check(
      reported_as("q_angle_axis",
                  math::from_axis_angle(math::Vec3(1.0F, 1.0F, 0.0F), 1.2F)) &&
          reported_as("q_from_to", math::from_to(a, b)) &&
          reported_as("q_rotate", math::rotate_vector(a, q)) &&
          reported_as("q_compose", math::mul(q, q2)) &&
          reported_as("q_slerp", math::slerp(q, q2, 0.4F)) &&
          reported_as("q_nlerp", math::nlerp(q, q2, 0.4F)) &&
          reported_as("q_rotate_towards", math::rotate_towards(q, q2, 0.2F)) &&
          reported_as("q_angle", math::angle(q, q2)) &&
          reported_as("q_inverse", math::inverse(q)) &&
          reported_as("q_forward", math::forward(q)) &&
          reported_as("q_up", math::up(q)) &&
          reported_as("q_right", math::right(q)) &&
          reported_as("q_look", look),
      "quat operations are the engine's, bit for bit");

  ctx.check(sc::call_script_function("check_value_rules"),
            "values are immutable, typed, and built and compared as documented");
  ctx.check(sc::call_script_function("setup_scene"),
            "a scene built with vec3 arguments");
  world->begin_transform_phase();
  world->end_frame_phase();
  ctx.check(sc::call_script_function("check_bindings"),
            "transform, rotation and gravity bindings take values and loose "
            "numbers, and refuse the wrong type");
  ctx.check(sc::call_script_function("check_queries"),
            "the physics queries take values and loose numbers, mixed too");

  sc::shutdown_scripting();
  engine::core::log_unregister_sink(&capture, nullptr);
  engine::core::shutdown_logging();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("lua_math_values");
}
