// Verifies the entity-script bindings' Lua state alias is cleared by the
// VM's owner (issue #318, contract #168): after shutdown_scripting the
// alias must not still point at the destroyed lua_State, so an
// entity-script dispatch becomes a no-op instead of running into freed
// memory. Also pins the repeated-lifecycle boundary — a second
// initialize_scripting re-arms dispatch rather than inheriting a stale
// alias — since a cleared alias is only correct if it is re-established.
//
// Each case is staged so the alias guard is the thing under test. Several
// of these entry points carry a second guard on the runtime binding,
// which shutdown_scripting also clears, so a case that left the binding
// down would return on that sibling guard and prove nothing about the
// alias; the preconditions below fail rather than let a case pass empty.
//
// One public entry point is deliberately absent: clear_entity_script_modules
// unrefs through the alias, but only over cached modules, and shutdown has
// necessarily emptied that cache — repopulating it would need the very VM
// that is gone. There is no state in which it can reach the freed VM, so
// no case here claims to cover it.
//
// The reload/file-changed path is absent for the same reason, and is named
// here because #318 singles it out as the regression to write. Both of its
// walks are empty after shutdown, so neither reaches the alias:
// check_script_reload iterates g_watchedScriptCount, which shutdown_scripting
// zeroes in its final loop; and dispatch_pending_entity_reloads (an internal
// helper, reached from the dispatch_entity_scripts_update that case 2 drives)
// iterates the module cache, which shutdown empties via
// reset_entity_script_bindings — the same clear that also drops its
// g_hasPendingEntityReloads flag, so its own guard short-circuits first.

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
/// Returns the dispatched entity, which the EndPlay case later queues for
/// destruction — an entity that began play is exactly what that dispatch
/// needs to reach the alias.
rt::Entity test_dispatch_is_live_before_shutdown(
    engine::tests::TestContext &ctx, rt::World *world) {
  const rt::Entity entity = make_scripted_entity(world);
  ctx.check(entity != rt::kInvalidEntity, "live: create scripted entity");

  run_begin_play_phase(world);
  ctx.check(fired_count_is(1), "live: on_begin_play was delivered");
  ctx.check(world->begin_play_pending_count() == 0U,
            "live: the entity was consumed by the dispatch");
  ctx.check(world->has_begun_play(entity), "live: the entity began play");
  return entity;
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

/// The four entry points that read the world from the runtime binding
/// rather than an argument. Each returns early on
/// `(g_state == nullptr) || (runtime_binding().world == nullptr)`, so a
/// dispatch proceeds only when both are non-null — and shutdown_scripting
/// clears the binding as well as the alias, so with the binding down they
/// return on the sibling guard and say nothing about the alias.
///
/// Rebinding the runtime is what makes the alias decisive here, and it is
/// #318's own failure scenario rather than a contrived state: the binding
/// is run-tier and the VM engine-tier, so a teardown-ordering change (or
/// a late notification) leaves exactly this pairing. Past the guard each
/// of the four dereferences the alias directly via arm_debug_lua_hook and
/// then loads a module through it.
///
/// dispatch_entity_scripts_start is the observable one: it calls
/// mark_begin_play_done on a successful dispatch, so a guard that failed
/// to hold consumes the pending entity.
void test_bound_runtime_after_shutdown_is_a_no_op(
    engine::tests::TestContext &ctx, rt::World *world,
    engine::core::ServiceLocator &serviceLocator) {
  rt::bind_scripting_runtime(world, serviceLocator);

  const rt::Entity entity = make_scripted_entity(world);
  ctx.check(entity != rt::kInvalidEntity, "bound runtime: create entity");
  const std::size_t pendingBefore = world->begin_play_pending_count();
  ctx.check(pendingBefore > 0U,
            "bound runtime: there is pending work to dispatch");

  sc::dispatch_entity_scripts_start();
  sc::dispatch_entity_scripts_update(1.0F / 60.0F);
  sc::dispatch_entity_scripts_end();
  sc::dispatch_entity_scripts_end_for_transition();

  ctx.check(world->begin_play_pending_count() == pendingBefore,
            "bound runtime: no dispatch consumed the pending entity");
}

/// dispatch_entity_scripts_end_play takes its world as an argument, so
/// the binding does not gate it — but it only reaches the alias for an
/// entity that both began play and is queued for deferred destruction.
/// Without that work the walk is zero-trip and proves nothing, so the
/// entity dispatched while the VM was alive is queued here and the
/// preconditions refuse to let the case pass as empty.
///
/// Driven through a real frame walk because that is what produces the
/// state: destroy_entity defers during Simulation precisely so EndPlay
/// fires for the subtree, and the EndPlay phase is entered from Render.
void test_end_play_after_shutdown_is_a_no_op(engine::tests::TestContext &ctx,
                                             rt::World *world,
                                             rt::Entity begunEntity) {
  world->begin_update_phase();
  ctx.check(world->destroy_entity(begunEntity),
            "end play: the entity is queued for destruction");
  ctx.check(world->pending_destroy_count() > 0U,
            "end play: the destroy deferred rather than running now");
  ctx.check(world->has_begun_play(begunEntity),
            "end play: the queued entity began play, so it would dispatch");

  world->begin_transform_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();
  world->begin_end_play_phase();
  sc::dispatch_entity_scripts_end_play(world);
  world->end_end_play_phase();
  world->end_frame_phase();
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
  const rt::Entity begunEntity =
      test_dispatch_is_live_before_shutdown(ctx, world.get());

  // The VM dies while the World lives on, which is the ordering the
  // finding is about: engine-tier teardown does not take run-tier state
  // with it, so the alias is what must be cleared.
  sc::clear_entity_script_modules();
  sc::shutdown_scripting();

  test_begin_play_after_shutdown_is_a_no_op(ctx, world.get());
  test_bound_runtime_after_shutdown_is_a_no_op(ctx, world.get(),
                                               serviceLocator);
  test_end_play_after_shutdown_is_a_no_op(ctx, world.get(), begunEntity);

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
