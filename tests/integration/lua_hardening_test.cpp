// Regression tests for Lua dispatch hardening: the latched CPU instruction
// budget (a wedge script cannot refill its budget through pcall recovery or
// fresh coroutines, and the latch clears at the next dispatch boundary),
// panic-safe engine-to-Lua dispatch under hostile metatables, and script
// dispatch surviving synchronous entity destruction from callbacks.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScript = "lua_hardening_test.lua";

/// Writes the test script file.
bool write_script(const char *code) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, kTempScript, "w") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(kTempScript, "w");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fputs(code, f);
  std::fclose(f);
  return true;
}

void remove_script() noexcept { std::remove(kTempScript); }

/// Owns one scripting session bound to a fresh world for a test case.
struct ScriptingSession final {
  std::unique_ptr<engine::runtime::World> world;
  engine::core::ServiceLocator serviceLocator{};
  bool ok = false;

  ScriptingSession() noexcept {
    if (!engine::scripting::initialize_scripting()) {
      return;
    }
    world = std::unique_ptr<engine::runtime::World>(
        new (std::nothrow) engine::runtime::World());
    if (world == nullptr) {
      return;
    }
    engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
    engine::scripting::set_sandbox_enabled(true);
    ok = true;
  }

  ~ScriptingSession() noexcept {
    engine::scripting::set_instruction_limit(1000000);
    engine::scripting::clear_entity_script_modules();
    engine::scripting::shutdown_scripting();
  }
};

// -----------------------------------------------------------------------
// N-13: a pcall-wrapped busy loop must not wedge the dispatch — once the
// instruction budget is exhausted the whole load_script call fails.
// -----------------------------------------------------------------------
bool test_pcall_wedge_terminates() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }
  engine::scripting::set_instruction_limit(10000);

  const char *code = "while true do\n"
                     "  pcall(function() while true do end end)\n"
                     "end\n";
  if (!write_script(code)) {
    return false;
  }
  const bool loadOk = engine::scripting::load_script(kTempScript);
  remove_script();
  return !loadOk;
}

// -----------------------------------------------------------------------
// N-13: spawning fresh coroutines must not refill the budget — the latch
// is shared across threads, so the resume loop terminates promptly.
// -----------------------------------------------------------------------
bool test_coroutine_wedge_terminates() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }
  engine::scripting::set_instruction_limit(10000);

  const char *code =
      "while true do\n"
      "  local co = coroutine.create(function() while true do end end)\n"
      "  coroutine.resume(co)\n"
      "end\n";
  if (!write_script(code)) {
    return false;
  }
  const bool loadOk = engine::scripting::load_script(kTempScript);
  remove_script();
  return !loadOk;
}

// -----------------------------------------------------------------------
// N-13 boundary: the exhaustion latch clears at the next dispatch, so a
// well-behaved script still runs after a wedged one was terminated.
// -----------------------------------------------------------------------
bool test_budget_resets_at_next_dispatch() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }
  engine::scripting::set_instruction_limit(10000);

  const char *wedge = "while true do\n"
                      "  pcall(function() while true do end end)\n"
                      "end\n";
  if (!write_script(wedge) || engine::scripting::load_script(kTempScript)) {
    remove_script();
    return false;
  }

  const char *sane = "hardening_marker = 41\n"
                     "hardening_marker = hardening_marker + 1\n"
                     "function verify_marker()\n"
                     "  if hardening_marker ~= 42 then error('marker') end\n"
                     "end\n";
  if (!write_script(sane)) {
    return false;
  }
  const bool loadOk = engine::scripting::load_script(kTempScript) &&
                      engine::scripting::call_script_function("verify_marker");
  remove_script();
  return loadOk;
}

/// Installs a hostile _G metatable whose __index raises from any lookup of
/// an undefined global, then defines a sanity probe for later dispatches.
bool install_hostile_global_metatable() noexcept {
  const char *code =
      "boom_count = 0\n"
      "setmetatable(_G, { __index = function() error('boom') end })\n"
      "function probe_after_boom()\n"
      "  boom_count = boom_count + 1\n"
      "end\n";
  return write_script(code) && engine::scripting::load_script(kTempScript) &&
         engine::scripting::call_script_function("probe_after_boom");
}

// -----------------------------------------------------------------------
// N-04: a hostile _G metatable must not reach lua_atpanic — the collision
// dispatch looks up the global on_collision fallback under protection.
// -----------------------------------------------------------------------
bool test_collision_dispatch_survives_hostile_metatable() noexcept {
  ScriptingSession session{};
  if (!session.ok || !install_hostile_global_metatable()) {
    return false;
  }

  const std::uint32_t pairData[2] = {1U, 2U};
  engine::scripting::dispatch_physics_callbacks(pairData, 1U);
  remove_script();
  return engine::scripting::call_script_function("probe_after_boom");
}

std::size_t hostile_fired_event_count() noexcept { return 1U; }

bool hostile_fired_event_at(std::size_t index, engine::core::Entity *outEntity,
                            const char **outName) noexcept {
  if ((index != 0U) || (outEntity == nullptr) || (outName == nullptr)) {
    return false;
  }
  *outEntity = engine::core::Entity{};
  *outName = "footstep";
  return true;
}

// -----------------------------------------------------------------------
// N-04: the animation-event dispatch looks up the global on_anim_event
// fallback under protection, so the hostile metatable cannot abort.
// -----------------------------------------------------------------------
bool test_anim_event_dispatch_survives_hostile_metatable() noexcept {
  ScriptingSession session{};
  if (!session.ok || !install_hostile_global_metatable()) {
    return false;
  }

  engine::scripting::AnimationScriptBridge bridge{};
  bridge.firedEventCount = &hostile_fired_event_count;
  bridge.firedEventAt = &hostile_fired_event_at;
  engine::scripting::set_animation_script_bridge(bridge);
  engine::scripting::dispatch_animation_event_callbacks();
  engine::scripting::set_animation_script_bridge(
      engine::scripting::AnimationScriptBridge{});

  remove_script();
  return engine::scripting::call_script_function("probe_after_boom");
}

// -----------------------------------------------------------------------
// N-04: a script module with a hostile metatable on its returned table
// raises from the on_tick field lookup — the tick dispatch must log and
// survive instead of aborting, and the entity is marked faulted.
// -----------------------------------------------------------------------
bool test_tick_dispatch_survives_hostile_module_metatable() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }

  const char *code =
      "return setmetatable({}, { __index = function() error('boom') end })\n";
  if (!write_script(code)) {
    return false;
  }

  const engine::runtime::Entity entity = session.world->create_entity();
  engine::runtime::ScriptComponent sc{};
  std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kTempScript);
  if ((entity == engine::runtime::kInvalidEntity) ||
      !session.world->add_script_component(entity, sc)) {
    remove_script();
    return false;
  }

  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  remove_script();

  const char *sane = "function verify_alive_after_module_boom()\n"
                     "end\n";
  return write_script(sane) && engine::scripting::load_script(kTempScript) &&
         engine::scripting::call_script_function(
             "verify_alive_after_module_boom");
}

} // namespace

/// Runs this executable or test program.
int main() {
  int failures = 0;

  struct TestCase {
    const char *name;
    bool (*fn)();
  };

  const TestCase tests[] = {
      {"pcall_wedge_terminates", test_pcall_wedge_terminates},
      {"coroutine_wedge_terminates", test_coroutine_wedge_terminates},
      {"budget_resets_at_next_dispatch", test_budget_resets_at_next_dispatch},
      {"collision_dispatch_survives_hostile_metatable",
       test_collision_dispatch_survives_hostile_metatable},
      {"anim_event_dispatch_survives_hostile_metatable",
       test_anim_event_dispatch_survives_hostile_metatable},
      {"tick_dispatch_survives_hostile_module_metatable",
       test_tick_dispatch_survives_hostile_module_metatable},
  };

  for (const auto &tc : tests) {
    std::printf("  lua_hardening_test::%s ... ", tc.name);
    std::fflush(stdout);
    if (tc.fn()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  return (failures == 0) ? 0 : 1;
}
