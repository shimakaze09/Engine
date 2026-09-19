// Verifies the Lua ray queries take the sweeps' optional skip entity
// (#537 item 3): engine.raycast and engine.raycast_all exclude that
// entity's colliders and the compound colliders it owns, so a ground
// probe from a character's root no longer hits its own child at t = 0,
// while an omitted skip keeps every hit and a stale handle fails the call.

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

constexpr const char *kScriptPath = "script_raycast_skip_test.lua";

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

// The scene: a dynamic root at the origin with a child collider at x=2
// (a compound body) and a static wall at x=6. Every ray runs down +X.
constexpr const char *kScript =
    "g_root = nil\n"
    "g_child = nil\n"
    "g_wall = nil\n"
    "local function count(t)\n"
    "    local n = 0\n"
    "    for _ in pairs(t) do n = n + 1 end\n"
    "    return n\n"
    "end\n"
    "function setup_scene()\n"
    "    g_root = engine.spawn_entity()\n"
    "    g_child = engine.spawn_entity()\n"
    "    g_wall = engine.spawn_entity()\n"
    "    if g_root == nil or g_child == nil or g_wall == nil then\n"
    "        error('spawn failed')\n"
    "    end\n"
    "    if not engine.add_collider(g_root, 0.5, 0.5, 0.5) then\n"
    "        error('root collider failed')\n"
    "    end\n"
    "    if not engine.add_rigid_body(g_root, 1.0) then\n"
    "        error('root body failed')\n"
    "    end\n"
    "    engine.set_position(g_child, 2.0, 0.0, 0.0)\n"
    "    if not engine.add_collider(g_child, 0.5, 0.5, 0.5) then\n"
    "        error('child collider failed')\n"
    "    end\n"
    "    if not engine.set_parent(g_child, g_root) then\n"
    "        error('set_parent failed')\n"
    "    end\n"
    "    engine.set_position(g_wall, 6.0, 0.0, 0.0)\n"
    "    if not engine.add_collider(g_wall, 0.5, 2.0, 2.0) then\n"
    "        error('wall collider failed')\n"
    "    end\n"
    "end\n"
    "function verify_skip()\n"
    "    if engine.raycast(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0) == g_wall\n"
    "        then error('an unskipped ray from the root hits its body') end\n"
    "    local hit, dist = engine.raycast(0.0, 0.0, 0.0, 1.0, 0.0, 0.0,\n"
    "                                     20.0, g_root)\n"
    "    if hit ~= g_wall then\n"
    "        error('skipping the root must skip its compound child too')\n"
    "    end\n"
    "    if math.abs(dist - 5.5) > 0.0001 then\n"
    "        error('skipped ray distance ' .. tostring(dist))\n"
    "    end\n"
    "    if count(engine.raycast_all(-3.0, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0))\n"
    "        ~= 3 then error('unskipped raycast_all must see all three') end\n"
    "    local hits = engine.raycast_all(-3.0, 0.0, 0.0, 1.0, 0.0, 0.0,\n"
    "                                    20.0, nil, g_root)\n"
    "    if count(hits) ~= 1 or hits[1].entity ~= g_wall then\n"
    "        error('raycast_all skip must leave only the wall')\n"
    "    end\n"
    "    local masked = engine.raycast_all(-3.0, 0.0, 0.0, 1.0, 0.0, 0.0,\n"
    "                                      20.0, 0xFFFFFFFF, g_root)\n"
    "    if count(masked) ~= 1 then\n"
    "        error('mask and skip compose')\n"
    "    end\n"
    "end\n"
    "function verify_stale_skip_fails()\n"
    "    local probe = engine.spawn_entity()\n"
    "    if probe == nil then error('spawn probe failed') end\n"
    "    if not engine.destroy_entity(probe) then error('destroy failed') end\n"
    "    if engine.raycast(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0, probe) ~= nil\n"
    "        then error('a stale skip handle must fail the raycast') end\n"
    "    if count(engine.raycast_all(-3.0, 0.0, 0.0, 1.0, 0.0, 0.0, 20.0,\n"
    "                                nil, probe)) ~= 0\n"
    "        then error('a stale skip handle must fail raycast_all') end\n"
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

  ctx.check(sc::call_script_function("verify_skip"),
            "raycast and raycast_all skip the entity and its compound body");
  ctx.check(sc::call_script_function("verify_stale_skip_fails"),
            "a stale skip handle fails the query instead of being ignored");

  sc::shutdown_scripting();
  static_cast<void>(std::remove(kScriptPath));
  return ctx.finish("script_raycast_skip");
}
