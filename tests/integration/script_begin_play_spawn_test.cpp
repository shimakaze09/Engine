// Verifies the begin-play spawn contract through the production dispatch:
// on_begin_play runs in the World's Input phase, so a callback may spawn a
// scripted entity, and that entity begins play in a later pass of the same
// dispatch. A spawn chain deeper than the per-dispatch pass bound finishes
// on the following dispatches with every link delivered exactly once, and
// two runs of the same chain on fresh Worlds reach the same state hash.

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

constexpr const char *kChainPath = "bp_spawn_chain.lua";
constexpr const char *kDriverPath = "bp_spawn_driver.lua";

// Each link counts its begin play and, until the chain reaches its target
// length, spawns the next link carrying this same script. A refused spawn
// or attach raises, which faults the entity and leaves the count short.
constexpr const char *kChainScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    chain_fired = chain_fired + 1\n"
    "    if chain_fired < chain_target then\n"
    "        local e = engine.spawn_shape('cube', chain_fired, 0, 0)\n"
    "        if e == nil then error('spawn refused in on_begin_play') end\n"
    "        if not engine.add_script_component(e, 'bp_spawn_chain.lua') then\n"
    "            error('attach refused in on_begin_play')\n"
    "        end\n"
    "    end\n"
    "end\n"
    "return M\n";

bool write_file_at(const char *path, const char *contents) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, length, file) == length;
  return (std::fclose(file) == 0) && ok;
}

/// Runs a Lua chunk in the global environment.
bool run_lua(const char *code) noexcept {
  return write_file_at(kDriverPath, code) && sc::load_script(kDriverPath);
}

/// Resets the chain's Lua counters for a chain of `target` links.
bool start_chain(int target) noexcept {
  char code[128] = {};
  std::snprintf(code, sizeof(code), "chain_fired = 0\nchain_target = %d\n",
                target);
  return run_lua(code);
}

/// Fails the Lua call unless the chain has fired exactly `expected` times.
bool chain_fired_is(int expected) noexcept {
  char code[192] = {};
  std::snprintf(code, sizeof(code),
                "if chain_fired ~= %d then\n"
                "    error('chain_fired ' .. tostring(chain_fired))\n"
                "end\n",
                expected);
  return run_lua(code);
}

/// Adds the chain's first link to `world`.
bool add_chain_root(rt::World &world) noexcept {
  const rt::Entity root = world.create_scene_object(rt::Transform{});
  rt::ScriptComponent script{};
  std::snprintf(script.scriptPath, sizeof(script.scriptPath), "%s", kChainPath);
  return (root != rt::kInvalidEntity) &&
         world.add_script_component(root, script);
}

/// The World scripting is bound to. It outlives every test, since the
/// scripting shutdown still reaches the bound World.
std::unique_ptr<rt::World> g_world{};

/// Binds scripting to a fresh World holding one chain root, releasing the
/// previous one only after the rebind; null on failure.
rt::World *fresh_world(engine::core::ServiceLocator &locator) {
  std::unique_ptr<rt::World> world(new (std::nothrow) rt::World());
  if (world == nullptr) {
    return nullptr;
  }
  sc::clear_entity_script_modules();
  rt::bind_scripting_runtime(world.get(), locator);
  g_world = std::move(world);
  return add_chain_root(*g_world) ? g_world.get() : nullptr;
}

/// EXPECTATION: a five-link chain, each link spawned by the previous
/// link's on_begin_play, is delivered whole by one dispatch.
void test_chain_in_one_dispatch(engine::tests::TestContext &ctx,
                                engine::core::ServiceLocator &locator) {
  rt::World *world = fresh_world(locator);
  ctx.check(world != nullptr, "bind a fresh world");
  if (world == nullptr) {
    return;
  }
  ctx.check(start_chain(5), "start a five-link chain");
  sc::dispatch_entity_scripts_begin_play(world);
  ctx.check(chain_fired_is(5),
            "every link spawned from on_begin_play began play in the same "
            "dispatch");
  ctx.check(world->alive_entity_count() == 5U, "five links are alive");
  ctx.check(world->begin_play_pending_count() == 0U, "no link is left pending");
}

/// EXPECTATION: a chain longer than one dispatch's pass bound keeps
/// going on the next dispatches, each link delivered exactly once.
void test_chain_past_the_pass_bound(engine::tests::TestContext &ctx,
                                    engine::core::ServiceLocator &locator) {
  rt::World *world = fresh_world(locator);
  ctx.check(world != nullptr, "bind a fresh world");
  if (world == nullptr) {
    return;
  }
  ctx.check(start_chain(20), "start a twenty-link chain");
  sc::dispatch_entity_scripts_begin_play(world);
  ctx.check(chain_fired_is(8), "one dispatch delivers eight passes");
  ctx.check(world->begin_play_pending_count() == 1U,
            "the ninth link waits for the next dispatch");
  sc::dispatch_entity_scripts_begin_play(world);
  sc::dispatch_entity_scripts_begin_play(world);
  ctx.check(chain_fired_is(20), "the chain completes over three dispatches");
  ctx.check(world->alive_entity_count() == 20U, "twenty links are alive");
  sc::dispatch_entity_scripts_begin_play(world);
  ctx.check(chain_fired_is(20), "no link begins play twice");
}

/// EXPECTATION: the same chain on two fresh Worlds reaches the same state
/// hash, so spawning from on_begin_play is deterministic.
void test_chain_is_deterministic(engine::tests::TestContext &ctx,
                                 engine::core::ServiceLocator &locator) {
  std::uint64_t hashes[2] = {};
  for (std::uint64_t &hash : hashes) {
    rt::World *world = fresh_world(locator);
    ctx.check(world != nullptr, "bind a fresh world");
    if (world == nullptr) {
      return;
    }
    ctx.check(start_chain(6), "start a six-link chain");
    sc::dispatch_entity_scripts_begin_play(world);
    ctx.check(chain_fired_is(6), "the six-link chain completes");
    hash = world->state_hash();
  }
  ctx.check(hashes[0] == hashes[1],
            "two runs of the chain reach the same state hash");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!sc::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    return 1;
  }
  engine::tests::TestContext ctx;
  engine::core::ServiceLocator locator{};
  ctx.check(write_file_at(kChainPath, kChainScript), "write the chain script");

  test_chain_in_one_dispatch(ctx, locator);
  test_chain_past_the_pass_bound(ctx, locator);
  test_chain_is_deterministic(ctx, locator);

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  g_world.reset();
  static_cast<void>(std::remove(kChainPath));
  static_cast<void>(std::remove(kDriverPath));
  return ctx.finish("script_begin_play_spawn");
}
