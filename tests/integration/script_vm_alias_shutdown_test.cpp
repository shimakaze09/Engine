// Verifies the entity-script bindings' Lua state alias is cleared by the
// VM's owner (issue #318, contract #168): after shutdown_scripting the
// alias must not still point at the destroyed lua_State, so every public
// entity-script entry point becomes a no-op instead of dispatching into
// freed memory. Also pins the repeated-lifecycle boundary — a second
// initialize_scripting re-arms dispatch rather than inheriting a stale
// alias — since a cleared alias is only correct if it is re-established.

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

constexpr const char *kModulePath = "vm_alias_module.lua";

/// A module whose on_begin_play is observable from Lua, so "dispatch
/// happened" is asserted by the callback firing rather than inferred.
constexpr const char *kModuleBody =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    bp_fired = (bp_fired or 0) + 1\n"
    "end\n"
    "function verify_fired(expected)\n"
    "    if (bp_fired or 0) ~= expected then\n"
    "        error('bp_fired ' .. tostring(bp_fired))\n"
    "    end\n"
    "end\n"
    "return M\n";

/// Writes contents to a relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(path, "wb");
  if (f == nullptr) {
    return false;
  }
#endif
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, f) == len);
  std::fclose(f);
  return ok;
}

/// Creates an entity carrying the test module's script path.
rt::Entity make_scripted_entity(rt::World *world) noexcept {
  const rt::Entity entity = world->create_entity();
  rt::ScriptComponent scriptComponent{};
  std::snprintf(scriptComponent.scriptPath,
                sizeof(scriptComponent.scriptPath), "%s", kModulePath);
  if (!world->add_script_component(entity, scriptComponent)) {
    return rt::kInvalidEntity;
  }
  return entity;
}

/// Runs one BeginPlay phase exactly as the pipeline does.
void run_begin_play_phase(rt::World *world) noexcept {
  world->begin_begin_play_phase();
  sc::dispatch_entity_scripts_begin_play(world);
  world->end_begin_play_phase();
}

/// Asserts the Lua-side delivery count, which only a live VM can answer;
/// after shutdown the counter is unreachable, so callers check the World.
bool fired_count_is(int expected) noexcept {
  char call[64] = {};
  std::snprintf(call, sizeof(call), "verify_fired(%d)", expected);
  constexpr const char *kCheckPath = "vm_alias_check.lua";
  if (!write_file_at(kCheckPath, call)) {
    return false;
  }
  const bool ok = sc::load_script(kCheckPath);
  static_cast<void>(std::remove(kCheckPath));
  return ok;
}

/// The live baseline: with the VM up, a scripted entity is dispatched.
/// Without this the post-shutdown assertions below would also hold for a
/// harness that never dispatches anything.
void test_dispatch_is_live_before_shutdown(engine::tests::TestContext &ctx,
                                           rt::World *world) {
  const rt::Entity entity = make_scripted_entity(world);
  ctx.check(entity != rt::kInvalidEntity, "live: create scripted entity");

  run_begin_play_phase(world);
  ctx.check(fired_count_is(1), "live: on_begin_play was delivered");
  ctx.check(world->begin_play_pending_count() == 0U,
            "live: the entity was consumed by the dispatch");
}

/// The finding: a BeginPlay dispatch issued after shutdown_scripting must
/// not reach the destroyed VM. The World outliving the VM is the ordinary
/// case — World is a run-tier object and the VM an engine-tier one — and
/// the world here is the caller's argument, not the runtime binding, so
/// clearing that binding does not guard this path.
///
/// On the unfixed revision the alias still names the freed lua_State, the
/// guard passes, and the dispatch walks into protected_load_chunk on
/// freed memory. The assertion is the World-side evidence: a no-op leaves
/// the entity pending, while any dispatch attempt consumes or corrupts it.
void test_begin_play_after_shutdown_is_a_no_op(
    engine::tests::TestContext &ctx, rt::World *world) {
  const rt::Entity entity = make_scripted_entity(world);
  ctx.check(entity != rt::kInvalidEntity, "post-shutdown: create entity");
  const std::size_t pendingBefore = world->begin_play_pending_count();
  ctx.check(pendingBefore > 0U, "post-shutdown: the entity is pending");

  run_begin_play_phase(world);

  ctx.check(world->begin_play_pending_count() == pendingBefore,
            "post-shutdown: BeginPlay dispatch left the entity pending");
}

/// The remaining public entity-script entry points on the same contract:
/// each guards on the same alias, so each must decline post-shutdown
/// rather than reach the freed VM. Driven with a World that has pending
/// begin-play and pending-destroy work, so a guard that failed to hold
/// would have something to walk into.
void test_remaining_entry_points_after_shutdown(
    engine::tests::TestContext &ctx, rt::World *world) {
  const rt::Entity entity = make_scripted_entity(world);
  ctx.check(entity != rt::kInvalidEntity, "entry points: create entity");
  const std::size_t pendingBefore = world->begin_play_pending_count();

  sc::dispatch_entity_scripts_start();
  sc::dispatch_entity_scripts_update(1.0F / 60.0F);
  sc::dispatch_entity_scripts_end();
  sc::dispatch_entity_scripts_end_play(world);
  sc::dispatch_entity_scripts_end_for_transition();
  // Unrefs cached module registry entries through the alias when it is
  // set, which is the module-clearing use-after-free in the finding.
  sc::clear_entity_script_modules();

  ctx.check(world->begin_play_pending_count() == pendingBefore,
            "entry points: no post-shutdown dispatch consumed the entity");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!write_file_at(kModulePath, kModuleBody)) {
    std::fprintf(stderr, "FAIL: write module file\n");
    return 1;
  }
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
  test_dispatch_is_live_before_shutdown(ctx, world.get());

  // The VM dies while the World lives on, which is the ordering the
  // finding is about: engine-tier teardown does not take run-tier state
  // with it, so the alias is what must be cleared.
  sc::clear_entity_script_modules();
  sc::shutdown_scripting();

  test_begin_play_after_shutdown_is_a_no_op(ctx, world.get());
  test_remaining_entry_points_after_shutdown(ctx, world.get());

  // Repeated lifecycle: a cleared alias must be re-established by the
  // next initialize_scripting, not left cleared, or the fix would trade
  // a use-after-free for a permanently dead dispatch.
  ctx.check(sc::initialize_scripting(), "restart: scripting initializes");
  rt::bind_scripting_runtime(world.get(), serviceLocator);
  const std::size_t pendingBeforeRestart = world->begin_play_pending_count();
  ctx.check(pendingBeforeRestart > 0U,
            "restart: entities are still pending from the dead session");
  run_begin_play_phase(world.get());
  ctx.check(world->begin_play_pending_count() == 0U,
            "restart: the pending entities were dispatched");
  // The restarted VM is a fresh one, so its counter starts at zero and
  // counts only this session's deliveries.
  ctx.check(fired_count_is(static_cast<int>(pendingBeforeRestart)),
            "restart: on_begin_play fired once per pending entity");

  sc::clear_entity_script_modules();
  sc::shutdown_scripting();
  static_cast<void>(std::remove(kModulePath));
  return ctx.finish("script_vm_alias_shutdown");
}
