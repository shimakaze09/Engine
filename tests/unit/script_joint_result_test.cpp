// Verifies the unified Lua joint-constructor failure contract (#100):
// every constructor returns nil for binding-level and native-level
// rejections (invalid handles, bad parameters, self-joints, full table)
// while successful ids stay round-trippable through limits and removal.
//
// Also verifies the matching mutator contract (#126): set_joint_limits and
// remove_joint join the constructors' nil-on-failure convention (true on
// success, nil — not false — on failure) so a script can tell a dropped
// write from an applied one, covering a live id, a native-level rejection
// (wrong joint type), a stale id after removal, an out-of-range id, and the
// unbound-service path.

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

constexpr const char *kScriptPath = "script_joint_result_test.lua";

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

constexpr const char *kScript =
    "g_a = nil\n"
    "g_b = nil\n"
    "function setup_bodies()\n"
    "    g_a = engine.spawn_entity()\n"
    "    g_b = engine.spawn_entity()\n"
    "    if g_a == nil or g_b == nil then error('spawn failed') end\n"
    "    engine.set_position(g_a, 0.0, 0.0, 0.0)\n"
    "    engine.set_position(g_b, 2.0, 0.0, 0.0)\n"
    "    if not engine.add_rigid_body(g_a, 1.0) then error('body a') end\n"
    "    if not engine.add_rigid_body(g_b, 1.0) then error('body b') end\n"
    "end\n"
    "local function expect_id(id, what)\n"
    "    if type(id) ~= 'number' or id == 0 then\n"
    "        error(what .. ' must return a joint id, got ' .. tostring(id))\n"
    "    end\n"
    "    return id\n"
    "end\n"
    "local function expect_nil(id, what)\n"
    "    if id ~= nil then\n"
    "        error(what .. ' must return nil, got ' .. tostring(id))\n"
    "    end\n"
    "end\n"
    "function verify_success_roundtrip()\n"
    "    local ids = {\n"
    "        expect_id(engine.add_distance_joint(g_a, g_b, 2.0), 'distance'),\n"
    "        expect_id(engine.add_hinge_joint(g_a, g_b, 1.0, 0.0, 0.0,\n"
    "                                         0.0, 1.0, 0.0), 'hinge'),\n"
    "        expect_id(engine.add_ball_socket_joint(g_a, g_b,\n"
    "                                               1.0, 0.0, 0.0), 'ball'),\n"
    "        expect_id(engine.add_slider_joint(g_a, g_b,\n"
    "                                          1.0, 0.0, 0.0), 'slider'),\n"
    "        expect_id(engine.add_spring_joint(g_a, g_b,\n"
    "                                          2.0, 50.0, 1.0), 'spring'),\n"
    "        expect_id(engine.add_fixed_joint(g_a, g_b), 'fixed'),\n"
    "    }\n"
    "    for _, id in ipairs(ids) do\n"
    "        engine.set_joint_limits(id, 0.0, 1.0)\n"
    "        engine.remove_joint(id)\n"
    "    end\n"
    "end\n"
    "function verify_binding_rejection()\n"
    "    -- A non-entity first argument fails in the binding layer.\n"
    "    expect_nil(engine.add_distance_joint(nil, g_b, 2.0), 'distance nil')\n"
    "    expect_nil(engine.add_hinge_joint(nil, g_b), 'hinge nil')\n"
    "    expect_nil(engine.add_ball_socket_joint(nil, g_b), 'ball nil')\n"
    "    expect_nil(engine.add_slider_joint(nil, g_b), 'slider nil')\n"
    "    expect_nil(engine.add_spring_joint(nil, g_b), 'spring nil')\n"
    "    expect_nil(engine.add_fixed_joint(nil, g_b), 'fixed nil')\n"
    "end\n"
    "function verify_native_rejection()\n"
    "    -- Self-joints are rejected by the native constructors.\n"
    "    expect_nil(engine.add_distance_joint(g_a, g_a, 2.0), 'distance self')\n"
    "    expect_nil(engine.add_hinge_joint(g_a, g_a, 0.0, 0.0, 0.0,\n"
    "                                      0.0, 1.0, 0.0), 'hinge self')\n"
    "    expect_nil(engine.add_ball_socket_joint(g_a, g_a), 'ball self')\n"
    "    expect_nil(engine.add_slider_joint(g_a, g_a), 'slider self')\n"
    "    expect_nil(engine.add_spring_joint(g_a, g_a), 'spring self')\n"
    "    expect_nil(engine.add_fixed_joint(g_a, g_a), 'fixed self')\n"
    "    -- Invalid parameters: negative distance, zero hinge/slider axes.\n"
    "    expect_nil(engine.add_distance_joint(g_a, g_b, -1.0),\n"
    "               'negative distance')\n"
    "    expect_nil(engine.add_hinge_joint(g_a, g_b, 0.0, 0.0, 0.0,\n"
    "                                      0.0, 0.0, 0.0), 'zero hinge axis')\n"
    "    expect_nil(engine.add_slider_joint(g_a, g_b, 0.0, 0.0, 0.0),\n"
    "               'zero slider axis')\n"
    "end\n"
    "function verify_capacity_rejection()\n"
    "    -- Fill the joint table; exhaustion must surface as nil, then\n"
    "    -- removal must make slots claimable again.\n"
    "    local ids = {}\n"
    "    for _ = 1, 4097 do\n"
    "        local id = engine.add_distance_joint(g_a, g_b, 2.0)\n"
    "        if id == nil then break end\n"
    "        ids[#ids + 1] = id\n"
    "    end\n"
    "    if #ids >= 4097 then error('joint table never reported full') end\n"
    "    expect_nil(engine.add_distance_joint(g_a, g_b, 2.0), 'table full')\n"
    "    for _, id in ipairs(ids) do\n"
    "        engine.remove_joint(id)\n"
    "    end\n"
    "    local reused = engine.add_distance_joint(g_a, g_b, 2.0)\n"
    "    expect_id(reused, 'post-removal claim')\n"
    "    engine.remove_joint(reused)\n"
    "end\n"
    "function verify_mutator_success_and_native_rejection()\n"
    "    local hinge = expect_id(engine.add_hinge_joint(g_a, g_b, 1.0, 0.0,\n"
    "        0.0, 0.0, 1.0, 0.0), 'hinge')\n"
    "    if engine.set_joint_limits(hinge, -0.5, 0.5) ~= true then\n"
    "        error('set_joint_limits on a live hinge must return true')\n"
    "    end\n"
    "    local distance = expect_id(engine.add_distance_joint(g_a, g_b, 2.0),\n"
    "                               'distance')\n"
    "    -- Distance joints have no limits: a native-level type rejection,\n"
    "    -- distinct from a stale/invalid id, must still report nil.\n"
    "    expect_nil(engine.set_joint_limits(distance, 0.0, 1.0),\n"
    "               'set_joint_limits on a non-hinge/slider joint')\n"
    "    if engine.remove_joint(hinge) ~= true then\n"
    "        error('remove_joint on a live joint must return true')\n"
    "    end\n"
    "    if engine.remove_joint(distance) ~= true then\n"
    "        error('remove_joint on a live joint must return true')\n"
    "    end\n"
    "end\n"
    "function verify_mutator_stale_id_rejected()\n"
    "    local id = expect_id(engine.add_hinge_joint(g_a, g_b, 1.0, 0.0, 0.0,\n"
    "        0.0, 1.0, 0.0), 'hinge')\n"
    "    if engine.remove_joint(id) ~= true then\n"
    "        error('remove_joint on a live joint must return true')\n"
    "    end\n"
    "    -- id is now stale: both mutators must report nil, not silently\n"
    "    -- no-op as they did before #126.\n"
    "    expect_nil(engine.set_joint_limits(id, -0.5, 0.5),\n"
    "               'set_joint_limits on a stale id')\n"
    "    expect_nil(engine.remove_joint(id), 'remove_joint on a stale id')\n"
    "end\n"
    "function verify_mutator_invalid_id_rejected()\n"
    "    expect_nil(engine.set_joint_limits(999999, 0.0, 1.0),\n"
    "               'set_joint_limits on an out-of-range id')\n"
    "    expect_nil(engine.remove_joint(999999),\n"
    "               'remove_joint on an out-of-range id')\n"
    "end\n"
    "function verify_mutator_unbound_service_rejected()\n"
    "    expect_nil(engine.set_joint_limits(1, 0.0, 1.0),\n"
    "               'set_joint_limits with no bound runtime')\n"
    "    expect_nil(engine.remove_joint(1),\n"
    "               'remove_joint with no bound runtime')\n"
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
  ctx.check(sc::call_script_function("setup_bodies"), "body setup");

  // Joints read composed physics transforms, so commit transforms first.
  world->begin_transform_phase();
  world->end_frame_phase();

  ctx.check(sc::call_script_function("verify_success_roundtrip"),
            "all six constructors return round-trippable ids");
  ctx.check(sc::call_script_function("verify_binding_rejection"),
            "binding-level rejection returns nil");
  ctx.check(sc::call_script_function("verify_native_rejection"),
            "native-level rejection returns nil");
  ctx.check(sc::call_script_function("verify_capacity_rejection"),
            "joint table exhaustion returns nil");
  ctx.check(sc::call_script_function("verify_mutator_success_and_native_rejection"),
            "set_joint_limits/remove_joint report true for a live joint, "
            "nil for a native-level type rejection");
  ctx.check(sc::call_script_function("verify_mutator_stale_id_rejected"),
            "set_joint_limits/remove_joint report nil for a stale id after "
            "removal");
  ctx.check(sc::call_script_function("verify_mutator_invalid_id_rejected"),
            "set_joint_limits/remove_joint report nil for an out-of-range id");

  // #126 unbound-service path: the mutators must fail closed (nil), not
  // crash, once the runtime binding is torn down.
  rt::unbind_scripting_runtime(serviceLocator);
  ctx.check(sc::call_script_function("verify_mutator_unbound_service_rejected"),
            "set_joint_limits/remove_joint report nil with no bound runtime");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("script_joint_result");
}
