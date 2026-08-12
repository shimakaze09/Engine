// Verifies the Lua sweep bindings' optional skip-entity argument (#99):
// a sweep starting inside the caller's own collider can exclude that body
// (including compound child colliders) and hit the next obstacle, while a
// stale or invalid skip handle is rejected safely.

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

constexpr const char *kScriptPath = "script_sweep_skip_test.lua";

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

// The scene: a dynamic self body at the origin with a compound child
// collider at x=2, and a wall at x=6. All sweeps run down +X.
constexpr const char *kScript =
    "g_self = nil\n"
    "g_wall = nil\n"
    "function setup_scene()\n"
    "    g_self = engine.spawn_entity()\n"
    "    if g_self == nil then error('spawn self failed') end\n"
    "    engine.set_position(g_self, 0.0, 0.0, 0.0)\n"
    "    if not engine.add_collider(g_self, 0.5, 0.5, 0.5) then\n"
    "        error('self collider failed')\n"
    "    end\n"
    "    if not engine.add_rigid_body(g_self, 1.0) then\n"
    "        error('self rigid body failed')\n"
    "    end\n"
    "    local child = engine.spawn_entity()\n"
    "    if child == nil then error('spawn child failed') end\n"
    "    engine.set_position(child, 2.0, 0.0, 0.0)\n"
    "    if not engine.set_parent(child, g_self) then\n"
    "        error('parent child failed')\n"
    "    end\n"
    "    if not engine.add_collider(child, 0.5, 0.5, 0.5) then\n"
    "        error('child collider failed')\n"
    "    end\n"
    "    g_wall = engine.spawn_entity()\n"
    "    if g_wall == nil then error('spawn wall failed') end\n"
    "    engine.set_position(g_wall, 6.0, 0.0, 0.0)\n"
    "    if not engine.add_collider(g_wall, 0.5, 2.0, 2.0) then\n"
    "        error('wall collider failed')\n"
    "    end\n"
    "end\n"
    "function verify_sphere_skip()\n"
    "    -- Without the skip the sweep starting inside the body hits itself.\n"
    "    local hit = engine.sweep_sphere(0.0, 0.0, 0.0, 0.5,\n"
    "                                    1.0, 0.0, 0.0, 20.0)\n"
    "    if hit ~= g_self then error('unskipped sweep must hit self') end\n"
    "    -- Skipping self must also skip the compound child and hit the\n"
    "    -- wall: contact at x=5.0, five units out.\n"
    "    local hit2, dist = engine.sweep_sphere(0.0, 0.0, 0.0, 0.5,\n"
    "                                           1.0, 0.0, 0.0, 20.0,\n"
    "                                           0xFFFFFFFF, g_self)\n"
    "    if hit2 == nil then error('skipped sphere sweep found no hit') end\n"
    "    if hit2 ~= g_wall then error('skipped sphere sweep must hit wall') end\n"
    "    if math.abs(dist - 5.0) > 0.001 then\n"
    "        error('skipped sphere sweep distance ' .. dist)\n"
    "    end\n"
    "end\n"
    "function verify_box_skip()\n"
    "    local hit = engine.sweep_box(0.0, 0.0, 0.0, 0.5, 0.5, 0.5,\n"
    "                                 1.0, 0.0, 0.0, 20.0)\n"
    "    if hit ~= g_self then error('unskipped box sweep must hit self') end\n"
    "    local hit2, dist = engine.sweep_box(0.0, 0.0, 0.0, 0.5, 0.5, 0.5,\n"
    "                                        1.0, 0.0, 0.0, 20.0,\n"
    "                                        0xFFFFFFFF, g_self)\n"
    "    if hit2 == nil then error('skipped box sweep found no hit') end\n"
    "    if hit2 ~= g_wall then error('skipped box sweep must hit wall') end\n"
    "    if math.abs(dist - 5.0) > 0.001 then\n"
    "        error('skipped box sweep distance ' .. dist)\n"
    "    end\n"
    "end\n"
    "function verify_invalid_skip()\n"
    "    local stale = engine.spawn_entity()\n"
    "    if stale == nil then error('spawn stale failed') end\n"
    "    if not engine.destroy_entity(stale) then\n"
    "        error('destroy stale failed')\n"
    "    end\n"
    "    -- A stale handle and a non-handle number are both rejected.\n"
    "    if engine.sweep_sphere(0.0, 0.0, 0.0, 0.5, 1.0, 0.0, 0.0, 20.0,\n"
    "                           0xFFFFFFFF, stale) ~= nil then\n"
    "        error('stale sphere skip must be rejected')\n"
    "    end\n"
    "    if engine.sweep_box(0.0, 0.0, 0.0, 0.5, 0.5, 0.5,\n"
    "                        1.0, 0.0, 0.0, 20.0,\n"
    "                        0xFFFFFFFF, stale) ~= nil then\n"
    "        error('stale box skip must be rejected')\n"
    "    end\n"
    "    if engine.sweep_sphere(0.0, 0.0, 0.0, 0.5, 1.0, 0.0, 0.0, 20.0,\n"
    "                           0xFFFFFFFF, 'not-an-entity') ~= nil then\n"
    "        error('non-handle sphere skip must be rejected')\n"
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

  ctx.check(sc::call_script_function("verify_sphere_skip"),
            "sphere sweep skips self and compound child");
  ctx.check(sc::call_script_function("verify_box_skip"),
            "box sweep skips self and compound child");
  ctx.check(sc::call_script_function("verify_invalid_skip"),
            "stale or invalid skip handles are rejected");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("script_sweep_skip");
}
