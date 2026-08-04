// Regression tests for Lua dispatch hardening: the latched CPU instruction
// budget (a wedge script cannot refill its budget through pcall recovery or
// fresh coroutines, and the latch clears at the next dispatch boundary),
// panic-safe engine-to-Lua dispatch under hostile metatables, and script
// dispatch surviving synchronous entity destruction from callbacks.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <thread>

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

/// Writes contents to an arbitrary relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *f = nullptr;
#ifdef _WIN32
  if (fopen_s(&f, path, "w") != 0 || f == nullptr) {
    return false;
  }
#else
  f = std::fopen(path, "w");
  if (f == nullptr) {
    return false;
  }
#endif
  std::fputs(contents, f);
  std::fclose(f);
  return true;
}

// Minimal RuntimeServices wiring so Lua entity operations reach the world.
engine::runtime::World *g_testWorld = nullptr;

engine::runtime::WorldPhase get_phase(engine::runtime::World *w) noexcept {
  return (w != nullptr) ? w->current_phase()
                        : engine::runtime::WorldPhase::Input;
}

/// Creates a scene object for the Lua-facing spawn operation.
std::uint32_t create_scene_object(engine::runtime::World *w) noexcept {
  return (w != nullptr) ? w->create_scene_object().index : 0U;
}

/// Destroys the entity currently occupying the requested index.
bool destroy_entity(engine::runtime::World *w, std::uint32_t idx) noexcept {
  if (w == nullptr) {
    return false;
  }
  return w->destroy_entity(w->find_entity_by_index(idx));
}

std::uint32_t get_entity_count(engine::runtime::World *w) noexcept {
  return (w != nullptr) ? static_cast<std::uint32_t>(w->alive_entity_count())
                        : 0U;
}

/// Builds the requested runtime data for test services.
engine::scripting::RuntimeServices build_test_services() noexcept {
  engine::scripting::RuntimeServices svc{};
  svc.get_current_phase = &get_phase;
  svc.create_scene_object_op = &create_scene_object;
  svc.destroy_entity_op = &destroy_entity;
  svc.get_entity_count = &get_entity_count;
  return svc;
}

/// Owns one scripting session bound to a fresh world for a test case.
struct ScriptingSession final {
  std::unique_ptr<engine::runtime::World> world;
  engine::core::ServiceLocator serviceLocator{};
  engine::scripting::RuntimeServices services{};
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
    g_testWorld = world.get();
    engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
    services = build_test_services();
    engine::scripting::bind_runtime_services(&services, serviceLocator);
    engine::scripting::set_sandbox_enabled(true);
    ok = true;
  }

  ~ScriptingSession() noexcept {
    engine::scripting::set_instruction_limit(1000000);
    engine::scripting::clear_entity_script_modules();
    engine::scripting::shutdown_scripting();
    g_testWorld = nullptr;
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

/// Adds a script component pointing at path to a fresh scene entity.
engine::runtime::Entity add_scripted_entity(engine::runtime::World *world,
                                            const char *path) noexcept {
  const engine::runtime::Entity entity = world->create_entity();
  if (entity == engine::runtime::kInvalidEntity) {
    return engine::runtime::kInvalidEntity;
  }
  engine::runtime::ScriptComponent sc{};
  std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", path);
  if (!world->add_script_component(entity, sc)) {
    return engine::runtime::kInvalidEntity;
  }
  return entity;
}

// -----------------------------------------------------------------------
// N-14: a script's on_tick synchronously destroying another scripted
// entity must not skip surviving entities' ticks that frame (dense-array
// swap-and-pop moved the last component into the destroyed slot), and
// each surviving script must keep running its own module (distinct
// per-module counters would cross on cache pollution). Both orderings:
// victim before killer, and killer before victim.
// -----------------------------------------------------------------------
bool test_tick_destroy_no_skipped_ticks() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }

  const char *victim = "local M = {}\n"
                       "function M.on_begin_play(self)\n"
                       "  victim_handle = self\n"
                       "end\n"
                       "function M.on_tick(self, dt)\n"
                       "  victim_ticks = victim_ticks + 1\n"
                       "end\n"
                       "return M\n";
  const char *killer = "local M = {}\n"
                       "function M.on_tick(self, dt)\n"
                       "  killer_ticks = killer_ticks + 1\n"
                       "  if victim_handle ~= nil then\n"
                       "    engine.destroy_entity(victim_handle)\n"
                       "    victim_handle = nil\n"
                       "  end\n"
                       "end\n"
                       "return M\n";
  const char *counterA = "local M = {}\n"
                         "function M.on_tick(self, dt)\n"
                         "  a_ticks = a_ticks + 1\n"
                         "end\n"
                         "return M\n";
  const char *counterB = "local M = {}\n"
                         "function M.on_tick(self, dt)\n"
                         "  b_ticks = b_ticks + 1\n"
                         "end\n"
                         "return M\n";
  const char *prelude =
      "victim_ticks = 0\n"
      "killer_ticks = 0\n"
      "a_ticks = 0\n"
      "b_ticks = 0\n"
      "function verify_first_frame()\n"
      "  if victim_ticks ~= 1 then error('victim ' .. victim_ticks) end\n"
      "  if killer_ticks ~= 1 then error('killer ' .. killer_ticks) end\n"
      "  if a_ticks ~= 1 then error('a ' .. a_ticks) end\n"
      "  if b_ticks ~= 1 then error('b ' .. b_ticks) end\n"
      "end\n"
      "function verify_second_frame()\n"
      "  if victim_ticks ~= 1 then error('victim2 ' .. victim_ticks) end\n"
      "  if killer_ticks ~= 2 then error('killer2 ' .. killer_ticks) end\n"
      "  if a_ticks ~= 2 then error('a2 ' .. a_ticks) end\n"
      "  if b_ticks ~= 2 then error('b2 ' .. b_ticks) end\n"
      "end\n"
      "function verify_reversed_frame()\n"
      "  if victim_ticks ~= 0 then error('victimR ' .. victim_ticks) end\n"
      "  if killer_ticks ~= 1 then error('killerR ' .. killer_ticks) end\n"
      "  if a_ticks ~= 1 then error('aR ' .. a_ticks) end\n"
      "  if b_ticks ~= 1 then error('bR ' .. b_ticks) end\n"
      "end\n";

  if (!write_file_at("hardening_victim.lua", victim) ||
      !write_file_at("hardening_killer.lua", killer) ||
      !write_file_at("hardening_counter_a.lua", counterA) ||
      !write_file_at("hardening_counter_b.lua", counterB) ||
      !write_script(prelude) ||
      !engine::scripting::load_script(kTempScript)) {
    return false;
  }

  engine::runtime::World *world = session.world.get();
  engine::runtime::Entity firstWave[4] = {
      add_scripted_entity(world, "hardening_victim.lua"),
      add_scripted_entity(world, "hardening_killer.lua"),
      add_scripted_entity(world, "hardening_counter_a.lua"),
      add_scripted_entity(world, "hardening_counter_b.lua")};
  bool ok = (firstWave[0] != engine::runtime::kInvalidEntity) &&
            (firstWave[1] != engine::runtime::kInvalidEntity) &&
            (firstWave[2] != engine::runtime::kInvalidEntity) &&
            (firstWave[3] != engine::runtime::kInvalidEntity);

  if (ok) {
    world->begin_begin_play_phase();
    engine::scripting::dispatch_entity_scripts_begin_play(world);
    world->end_begin_play_phase();

    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    ok = engine::scripting::call_script_function("verify_first_frame");
  }

  if (ok) {
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    ok = engine::scripting::call_script_function("verify_second_frame");
  }

  if (ok) {
    for (const engine::runtime::Entity entity : firstWave) {
      static_cast<void>(world->destroy_entity(entity));
    }
    engine::scripting::clear_entity_script_modules();
    ok = engine::scripting::load_script(kTempScript) &&
         (add_scripted_entity(world, "hardening_killer.lua") !=
          engine::runtime::kInvalidEntity) &&
         (add_scripted_entity(world, "hardening_victim.lua") !=
          engine::runtime::kInvalidEntity) &&
         (add_scripted_entity(world, "hardening_counter_a.lua") !=
          engine::runtime::kInvalidEntity) &&
         (add_scripted_entity(world, "hardening_counter_b.lua") !=
          engine::runtime::kInvalidEntity);
  }

  if (ok) {
    world->begin_begin_play_phase();
    engine::scripting::dispatch_entity_scripts_begin_play(world);
    world->end_begin_play_phase();

    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    ok = engine::scripting::call_script_function("verify_reversed_frame");
  }

  std::remove("hardening_victim.lua");
  std::remove("hardening_killer.lua");
  std::remove("hardening_counter_a.lua");
  std::remove("hardening_counter_b.lua");
  remove_script();
  return ok;
}

// -----------------------------------------------------------------------
// #65 item 1: a first-time module load under an exhausted sandbox memory
// cap raises LUA_ERRMEM from C context (luaL_loadfile's chunk-name push,
// the module-table luaL_ref) — it must fail cleanly and the engine must
// keep running instead of hitting lua_atpanic/abort. Once memory is
// available again the module loads and ticks normally.
// -----------------------------------------------------------------------
bool test_module_load_survives_memory_exhaustion() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }

  const char *prelude = "memcap_ticks = 0\n"
                        "function verify_alive_after_memcap() end\n";
  const char *mod = "local M = {}\n"
                    "function M.on_tick(self, dt)\n"
                    "  memcap_ticks = memcap_ticks + 1\n"
                    "end\n"
                    "return M\n";
  if (!write_script(prelude) || !engine::scripting::load_script(kTempScript) ||
      !write_file_at("hardening_memcap.lua", mod)) {
    remove_script();
    return false;
  }

  const engine::runtime::Entity entity =
      add_scripted_entity(session.world.get(), "hardening_memcap.lua");
  bool ok = entity != engine::runtime::kInvalidEntity;

  if (ok) {
    engine::scripting::set_memory_limit(1024U);
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    engine::scripting::set_memory_limit(64U * 1024U * 1024U);
    ok = engine::scripting::call_script_function("verify_alive_after_memcap");
  }

  if (ok) {
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    const char *verify = "function verify_memcap_recovered()\n"
                         "  if memcap_ticks ~= 1 then\n"
                         "    error('ticks ' .. memcap_ticks)\n"
                         "  end\n"
                         "end\n";
    ok = write_script(verify) && engine::scripting::load_script(kTempScript) &&
         engine::scripting::call_script_function("verify_memcap_recovered");
  }

  std::remove("hardening_memcap.lua");
  remove_script();
  return ok;
}

// -----------------------------------------------------------------------
// #65 item 1: a hot-reload attempt (mtime changed) under an exhausted
// memory cap fails cleanly — the old module stays installed, the engine
// keeps running — and a later good save under normal memory reloads.
// -----------------------------------------------------------------------
bool test_module_reload_survives_memory_exhaustion() noexcept {
  ScriptingSession session{};
  if (!session.ok) {
    return false;
  }

  const char *prelude = "reload_memcap_marker = 0\n"
                        "function verify_alive_after_reload_memcap() end\n";
  const char *v1 = "local M = {}\n"
                   "function M.on_tick(self, dt)\n"
                   "  reload_memcap_marker = 1\n"
                   "end\n"
                   "return M\n";
  if (!write_script(prelude) || !engine::scripting::load_script(kTempScript) ||
      !write_file_at("hardening_reload_memcap.lua", v1)) {
    remove_script();
    return false;
  }

  const engine::runtime::Entity entity =
      add_scripted_entity(session.world.get(), "hardening_reload_memcap.lua");
  bool ok = entity != engine::runtime::kInvalidEntity;
  if (ok) {
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  }

  if (ok) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char *v2 = "local M = {}\n"
                     "function M.on_tick(self, dt)\n"
                     "  reload_memcap_marker = 2\n"
                     "end\n"
                     "return M\n";
    ok = write_file_at("hardening_reload_memcap.lua", v2);
  }

  if (ok) {
    engine::scripting::set_memory_limit(1024U);
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    engine::scripting::set_memory_limit(64U * 1024U * 1024U);
    ok = engine::scripting::call_script_function(
        "verify_alive_after_reload_memcap");
  }

  if (ok) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char *v3 = "local M = {}\n"
                     "function M.on_tick(self, dt)\n"
                     "  reload_memcap_marker = 3\n"
                     "end\n"
                     "return M\n";
    ok = write_file_at("hardening_reload_memcap.lua", v3);
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
    const char *verify = "function verify_reload_recovered()\n"
                         "  if reload_memcap_marker ~= 3 then\n"
                         "    error('marker ' .. reload_memcap_marker)\n"
                         "  end\n"
                         "end\n";
    ok = ok && write_script(verify) &&
         engine::scripting::load_script(kTempScript) &&
         engine::scripting::call_script_function("verify_reload_recovered");
  }

  std::remove("hardening_reload_memcap.lua");
  remove_script();
  return ok;
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
      {"tick_destroy_no_skipped_ticks", test_tick_destroy_no_skipped_ticks},
      {"module_load_survives_memory_exhaustion",
       test_module_load_survives_memory_exhaustion},
      {"module_reload_survives_memory_exhaustion",
       test_module_reload_survives_memory_exhaustion},
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
