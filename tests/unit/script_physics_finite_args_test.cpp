// Verifies the Lua physics bindings share one numeric-argument contract
// (#481): every float argument must be a finite number. A NaN or infinite
// query origin, direction, radius, half extent or distance fails the call
// (nil, false, or an empty table by the binding's existing failure shape)
// before it reaches the physics query; a non-finite collider, material,
// gravity or joint parameter is rejected with the previous value intact;
// an omitted optional argument still takes its documented default.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "../test_harness.h"
#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

namespace sc = engine::scripting;
namespace rt = engine::runtime;

constexpr const char *kScriptPath = "script_physics_finite_args_test.lua";

/// Writes contents to the temporary test script path.
bool write_script_file(const char *contents) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0 || file == nullptr) {
    return false;
  }
#else
  file = std::fopen(kScriptPath, "wb");
  if (file == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

// The scene: a static wall at x=6 and two dynamic bodies for the joint
// constructors. Every query runs from the origin down +X.
constexpr const char *kScript =
    "local nan = 0/0\n"
    "local inf = math.huge\n"
    "g_wall = nil\n"
    "g_a = nil\n"
    "g_b = nil\n"
    "local function count(t)\n"
    "    local n = 0\n"
    "    for _ in pairs(t) do n = n + 1 end\n"
    "    return n\n"
    "end\n"
    "function setup_scene()\n"
    "    g_wall = engine.spawn_entity()\n"
    "    if g_wall == nil then error('spawn wall failed') end\n"
    "    engine.set_position(g_wall, 6.0, 0.0, 0.0)\n"
    "    if not engine.add_collider(g_wall, 0.5, 2.0, 2.0) then\n"
    "        error('wall collider failed')\n"
    "    end\n"
    "    g_a = engine.spawn_entity()\n"
    "    g_b = engine.spawn_entity()\n"
    "    if g_a == nil or g_b == nil then error('spawn bodies failed') end\n"
    "    engine.set_position(g_a, 0.0, 10.0, 0.0)\n"
    "    engine.set_position(g_b, 2.0, 10.0, 0.0)\n"
    "    if not engine.add_collider(g_a, 0.5, 0.5, 0.5) then\n"
    "        error('a collider failed')\n"
    "    end\n"
    "    if not engine.add_collider(g_b, 0.5, 0.5, 0.5) then\n"
    "        error('b collider failed')\n"
    "    end\n"
    "    if not engine.add_rigid_body(g_a, 1.0) then\n"
    "        error('a body failed')\n"
    "    end\n"
    "    if not engine.add_rigid_body(g_b, 1.0) then\n"
    "        error('b body failed')\n"
    "    end\n"
    "end\n"
    // Guard: finite arguments still reach the queries and hit the wall.
    "function verify_finite_queries_hit()\n"
    "    local hit = engine.raycast(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0)\n"
    "    if hit ~= g_wall then error('finite raycast must hit wall') end\n"
    "    if count(engine.raycast_all(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0))\n"
    "        ~= 1 then error('finite raycast_all must hit wall') end\n"
    "    if count(engine.overlap_sphere(6.0, 0.0, 0.0, 1.0)) ~= 1 then\n"
    "        error('finite overlap_sphere must find wall')\n"
    "    end\n"
    "    if count(engine.overlap_box(6.0, 0.0, 0.0, 1.0, 1.0, 1.0)) ~= 1\n"
    "        then error('finite overlap_box must find wall') end\n"
    "    if engine.sweep_sphere(0.0, 0.0, 0.0, 0.5, 1.0, 0.0, 0.0, 20.0)\n"
    "        ~= g_wall then error('finite sweep_sphere must hit wall') end\n"
    "    if engine.sweep_box(0.0, 0.0, 0.0, 0.5, 0.5, 0.5,\n"
    "                        1.0, 0.0, 0.0, 20.0)\n"
    "        ~= g_wall then error('finite sweep_box must hit wall') end\n"
    "end\n"
    // Infinite extents and distances would otherwise reach the query and
    // hit; NaN components are the same contract at the other boundary.
    "function verify_non_finite_queries_fail()\n"
    "    if engine.raycast(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, inf) ~= nil then\n"
    "        error('infinite raycast distance must fail')\n"
    "    end\n"
    "    if engine.raycast(nan, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0) ~= nil then\n"
    "        error('NaN raycast origin must fail')\n"
    "    end\n"
    "    if engine.raycast(0.0, 0.0, 0.0, inf, 0.0, 0.0, 20.0) ~= nil then\n"
    "        error('infinite raycast direction must fail')\n"
    "    end\n"
    "    if count(engine.raycast_all(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, inf))\n"
    "        ~= 0 then error('infinite raycast_all distance must fail') end\n"
    "    if count(engine.raycast_all(0.0, nan, 0.0, 1.0, 0.0, 0.0, 20.0))\n"
    "        ~= 0 then error('NaN raycast_all origin must fail') end\n"
    "    if count(engine.overlap_sphere(0.0, 0.0, 0.0, inf)) ~= 0 then\n"
    "        error('infinite overlap_sphere radius must fail')\n"
    "    end\n"
    "    if count(engine.overlap_sphere(nan, 0.0, 0.0, 1.0)) ~= 0 then\n"
    "        error('NaN overlap_sphere center must fail')\n"
    "    end\n"
    "    if count(engine.overlap_box(0.0, 0.0, 0.0, inf, inf, inf)) ~= 0\n"
    "        then error('infinite overlap_box extents must fail') end\n"
    "    if count(engine.overlap_box(0.0, 0.0, nan, 1.0, 1.0, 1.0)) ~= 0\n"
    "        then error('NaN overlap_box center must fail') end\n"
    "    if engine.sweep_sphere(0.0, 0.0, 0.0, 0.5, 1.0, 0.0, 0.0, inf)\n"
    "        ~= nil then error('infinite sweep_sphere distance must fail') end\n"
    "    if engine.sweep_sphere(0.0, 0.0, 0.0, nan, 1.0, 0.0, 0.0, 20.0)\n"
    "        ~= nil then error('NaN sweep_sphere radius must fail') end\n"
    "    if engine.sweep_box(0.0, 0.0, 0.0, 0.5, 0.5, 0.5,\n"
    "                        1.0, 0.0, 0.0, inf)\n"
    "        ~= nil then error('infinite sweep_box distance must fail') end\n"
    "    if engine.sweep_box(0.0, 0.0, 0.0, inf, 0.5, 0.5,\n"
    "                        1.0, 0.0, 0.0, 20.0)\n"
    "        ~= nil then error('infinite sweep_box extent must fail') end\n"
    "end\n"
    "function verify_gravity()\n"
    "    engine.set_gravity(0.0, -5.0, 0.0)\n"
    "    engine.set_gravity(nan, -9.0, 0.0)\n"
    "    local gx, gy, gz = engine.get_gravity()\n"
    "    if gx ~= 0.0 or gy ~= -5.0 or gz ~= 0.0 then\n"
    "        error('NaN gravity must leave gravity unchanged')\n"
    "    end\n"
    "    engine.set_gravity(0.0, inf, 0.0)\n"
    "    gx, gy, gz = engine.get_gravity()\n"
    "    if gy ~= -5.0 then error('infinite gravity must be rejected') end\n"
    "    -- An omitted component is zero, as documented.\n"
    "    engine.set_gravity(1.0, -2.0)\n"
    "    gx, gy, gz = engine.get_gravity()\n"
    "    if gx ~= 1.0 or gy ~= -2.0 or gz ~= 0.0 then\n"
    "        error('omitted gravity component must default to zero')\n"
    "    end\n"
    "end\n"
    "function verify_collider_parameters()\n"
    "    if not engine.set_restitution(g_wall, 0.25) then\n"
    "        error('finite restitution must apply')\n"
    "    end\n"
    "    if engine.set_restitution(g_wall, nan) then\n"
    "        error('NaN restitution must be rejected')\n"
    "    end\n"
    "    if engine.get_restitution(g_wall) ~= 0.25 then\n"
    "        error('rejected restitution must leave the value intact')\n"
    "    end\n"
    "    if engine.set_friction(g_wall, inf, 0.5) then\n"
    "        error('infinite friction must be rejected')\n"
    "    end\n"
    "    local probe = engine.spawn_entity()\n"
    "    if probe == nil then error('spawn probe failed') end\n"
    "    if engine.add_capsule_collider(probe, nan, 0.5) then\n"
    "        error('NaN capsule half height must be rejected')\n"
    "    end\n"
    "    if engine.add_capsule_collider(probe, 1.0, inf) then\n"
    "        error('infinite capsule radius must be rejected')\n"
    "    end\n"
    "    if not engine.add_capsule_collider(probe, 1.0, 0.5) then\n"
    "        error('finite capsule must apply')\n"
    "    end\n"
    "end\n"
    "function verify_materials()\n"
    "    if engine.create_physics_material(nan, 0.5, 0.5) ~= nil then\n"
    "        error('NaN material friction must be rejected')\n"
    "    end\n"
    "    if engine.create_physics_material(0.5, 0.5, 0.5, inf) ~= nil then\n"
    "        error('infinite material density must be rejected')\n"
    "    end\n"
    "    local mat = engine.create_physics_material(0.1, 0.2, 0.3)\n"
    "    if mat == nil or mat.static_friction ~= 0.1 or mat.density ~= 1.0\n"
    "        then error('finite material keeps the caller values') end\n"
    "    mat.restitution = nan\n"
    "    if engine.set_collider_material(g_wall, mat) then\n"
    "        error('NaN material field must be rejected')\n"
    "    end\n"
    "    if engine.get_restitution(g_wall) ~= 0.25 then\n"
    "        error('rejected material must leave the collider intact')\n"
    "    end\n"
    "    mat.restitution = 0.75\n"
    "    if not engine.set_collider_material(g_wall, mat) then\n"
    "        error('finite material must apply')\n"
    "    end\n"
    "    if engine.get_restitution(g_wall) ~= 0.75 then\n"
    "        error('finite material must reach the collider')\n"
    "    end\n"
    "end\n"
    "function verify_joint_rejections()\n"
    "    if engine.add_distance_joint(g_a, g_b, nan) ~= nil then\n"
    "        error('NaN distance joint length must be rejected')\n"
    "    end\n"
    "    if engine.add_hinge_joint(g_a, g_b, 0.0, 0.0, 0.0, inf, 0.0, 0.0)\n"
    "        ~= nil then error('infinite hinge axis must be rejected') end\n"
    "    if engine.add_ball_socket_joint(g_a, g_b, nan, 0.0, 0.0) ~= nil\n"
    "        then error('NaN ball-socket pivot must be rejected') end\n"
    "    if engine.add_slider_joint(g_a, g_b, 1.0, nan, 0.0) ~= nil then\n"
    "        error('NaN slider axis must be rejected')\n"
    "    end\n"
    "    if engine.add_spring_joint(g_a, g_b, 1.0, inf, 1.0) ~= nil then\n"
    "        error('infinite spring stiffness must be rejected')\n"
    "    end\n"
    "end\n"
    // Omitted optionals still take their defaults and build a joint.
    "g_joint = nil\n"
    "function verify_default_joint_builds()\n"
    "    g_joint = engine.add_hinge_joint(g_a, g_b)\n"
    "    if g_joint == nil then error('default hinge joint must build') end\n"
    "end\n"
    "function verify_joint_limits()\n"
    "    if engine.set_joint_limits(g_joint, nan, 1.0) ~= nil then\n"
    "        error('NaN joint limit must be rejected')\n"
    "    end\n"
    "    if engine.set_joint_limits(g_joint, -1.0, 1.0) ~= true then\n"
    "        error('finite joint limits must apply')\n"
    "    end\n"
    "end\n";

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }

  auto world = std::unique_ptr<rt::World>(new (std::nothrow) rt::World());
  if (world == nullptr) {
    sc::shutdown_scripting();
    return 1;
  }
  engine::core::ServiceLocator serviceLocator{};
  rt::bind_scripting_runtime(world.get(), serviceLocator);

  engine::tests::TestContext ctx;
  ctx.check(write_script_file(kScript), "write test script");
  ctx.check(sc::load_script(kScriptPath), "load test script");
  ctx.check(sc::call_script_function("setup_scene"), "scene setup");

  // Queries consume the composed world pose, so commit transforms first.
  world->begin_transform_phase();
  world->end_frame_phase();

  ctx.check(sc::call_script_function("verify_finite_queries_hit"),
            "finite query arguments reach the physics queries");
  ctx.check(sc::call_script_function("verify_non_finite_queries_fail"),
            "non-finite query arguments fail before the physics query");
  ctx.check(sc::call_script_function("verify_gravity"),
            "non-finite gravity is rejected, omitted components default");
  ctx.check(sc::call_script_function("verify_collider_parameters"),
            "non-finite collider parameters are rejected");
  ctx.check(sc::call_script_function("verify_materials"),
            "non-finite material values are rejected");
  ctx.check(sc::call_script_function("verify_joint_rejections"),
            "non-finite joint parameters are rejected");
  ctx.check(sc::call_script_function("verify_default_joint_builds"),
            "omitted joint optionals still take their defaults");
  ctx.check(sc::call_script_function("verify_joint_limits"),
            "non-finite joint limits are rejected");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("script_physics_finite_args");
}
