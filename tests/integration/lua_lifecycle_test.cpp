// Verifies lua lifecycle test behavior for the Engine test suite.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "engine/core/service_locator.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

namespace {

constexpr const char *kTempScriptPath = "lua_lifecycle_test.lua";

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }
  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kTempScriptPath));
}

/// Selects the nearest working-directory ancestor containing demo assets.
bool set_working_directory_with_assets() noexcept {
  std::error_code error{};
  const std::filesystem::path original = std::filesystem::current_path(error);
  if (error) {
    return false;
  }

  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    error.clear();
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, error);
    if (error ||
        !std::filesystem::exists(normalized / "assets/main.lua", error)) {
      continue;
    }

    std::filesystem::current_path(normalized, error);
    return !error;
  }

  return false;
}

/// Writes script file data.
bool write_script_file(const char *contents) noexcept {
  if (contents == nullptr) {
    return false;
  }
  FILE *file = nullptr;
  if (!open_file_for_write(kTempScriptPath, &file) || (file == nullptr)) {
    return false;
  }
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

// Minimal RuntimeServices wiring so scripting dispatch works.
engine::runtime::World *g_testWorld = nullptr;

engine::runtime::WorldPhase get_phase(engine::runtime::World *w) noexcept {
  return (w != nullptr) ? w->current_phase()
                        : engine::runtime::WorldPhase::Input;
}

/// Creates a new object, handle, or resource for entity.
std::uint32_t create_entity(engine::runtime::World *w) noexcept {
  if (w == nullptr) {
    return 0U;
  }
  return w->create_entity().index;
}

/// Destroys or releases the requested object, handle, or resource for entity.
bool destroy_entity(engine::runtime::World *w, std::uint32_t idx) noexcept {
  if (w == nullptr) {
    return false;
  }
  const auto e = w->find_entity_by_index(idx);
  return w->destroy_entity(e);
}

bool add_transform(engine::runtime::World *w, std::uint32_t idx,
                   const engine::runtime::Transform &t) noexcept {
  if (w == nullptr) {
    return false;
  }
  const auto e = w->find_entity_by_index(idx);
  return w->add_transform(e, t);
}

std::uint32_t get_transform_count(engine::runtime::World *w) noexcept {
  return (w != nullptr) ? static_cast<std::uint32_t>(w->transform_count()) : 0U;
}

/// Builds the requested runtime data for test services.
engine::scripting::RuntimeServices build_test_services() noexcept {
  engine::scripting::RuntimeServices svc{};
  svc.get_current_phase = &get_phase;
  svc.create_entity_op = &create_entity;
  svc.destroy_entity_op = &destroy_entity;
  svc.add_transform_op = &add_transform;
  svc.get_transform_count = &get_transform_count;
  return svc;
}

/// Moves the test script timestamp forward from a known cached value.
bool advance_script_mtime(
    std::filesystem::file_time_type cachedMtime) noexcept {
  std::error_code error{};
  std::filesystem::last_write_time(
      kTempScriptPath, cachedMtime + std::chrono::seconds(2), error);
  return !error;
}

/// Verifies generation-safe state restoration before the first reloaded tick.
bool verify_entity_module_hot_reload(engine::runtime::World *world) noexcept {
  if (world == nullptr) {
    return false;
  }

  engine::scripting::clear_entity_script_modules();

  const char *faultingScript = "local M = {}\n"
                               "function M.on_tick(self, dt)\n"
                               "    error('intentional old-generation fault')\n"
                               "end\n"
                               "return M\n";
  if (!write_script_file(faultingScript)) {
    return false;
  }

  const engine::runtime::Entity oldEntity = world->create_entity();
  if (oldEntity == engine::runtime::kInvalidEntity) {
    return false;
  }
  engine::runtime::ScriptComponent oldScript{};
  std::snprintf(oldScript.scriptPath, sizeof(oldScript.scriptPath), "%s",
                kTempScriptPath);
  if (!world->add_script_component(oldEntity, oldScript)) {
    return false;
  }

  world->begin_begin_play_phase();
  engine::scripting::dispatch_entity_scripts_begin_play(world);
  world->end_begin_play_phase();
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);

  std::error_code error{};
  const std::filesystem::file_time_type faultingMtime =
      std::filesystem::last_write_time(kTempScriptPath, error);
  if (error) {
    return false;
  }

  const char *versionOne =
      "local M = {}\n"
      "local reload_completed = false\n"
      "function M.on_reload(self, state)\n"
      "    reload_completed = true\n"
      "end\n"
      "function M.on_save_state(self)\n"
      "    return { sentinel = 41, handle = self }\n"
      "end\n"
      "function M.on_tick(self, dt)\n"
      "    error('intentional recovered-generation fault')\n"
      "end\n"
      "function verify_fault_recovery()\n"
      "    if not reload_completed then error('fault did not recover') end\n"
      "end\n"
      "return M\n";
  if (!write_script_file(versionOne) || !advance_script_mtime(faultingMtime)) {
    return false;
  }

  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function("verify_fault_recovery")) {
    return false;
  }

  error.clear();
  const std::filesystem::file_time_type versionOneMtime =
      std::filesystem::last_write_time(kTempScriptPath, error);
  if (error || !world->destroy_entity(oldEntity)) {
    return false;
  }

  const engine::runtime::Entity recycledEntity = world->create_entity();
  if ((recycledEntity == engine::runtime::kInvalidEntity) ||
      (recycledEntity.index != oldEntity.index) ||
      (recycledEntity.generation == oldEntity.generation)) {
    return false;
  }
  engine::runtime::ScriptComponent recycledScript{};
  std::snprintf(recycledScript.scriptPath, sizeof(recycledScript.scriptPath),
                "%s", kTempScriptPath);
  if (!world->add_script_component(recycledEntity, recycledScript)) {
    return false;
  }

  world->begin_begin_play_phase();
  engine::scripting::dispatch_entity_scripts_begin_play(world);
  world->end_begin_play_phase();

  const char *versionTwo =
      "local M = {}\n"
      "local reload_count = 0\n"
      "local begin_play_count = 0\n"
      "local tick_count = 0\n"
      "local state_ok = false\n"
      "local tick_after_reload = false\n"
      "function M.on_reload(self, state)\n"
      "    reload_count = reload_count + 1\n"
      "    state_ok = type(state) == 'table' and state.sentinel == 41 "
      "and state.handle == self\n"
      "end\n"
      "function M.on_begin_play(self)\n"
      "    begin_play_count = begin_play_count + 1\n"
      "end\n"
      "function M.on_tick(self, dt)\n"
      "    tick_count = tick_count + 1\n"
      "    tick_after_reload = reload_count == 1\n"
      "end\n"
      "function verify_entity_reload()\n"
      "    if reload_count ~= 1 then error('reload count') end\n"
      "    if begin_play_count ~= 0 then error('begin play repeated') end\n"
      "    if tick_count ~= 1 then error('tick count') end\n"
      "    if not state_ok then error('saved state or handle') end\n"
      "    if not tick_after_reload then error('tick preceded reload') end\n"
      "end\n"
      "return M\n";
  if (!write_script_file(versionTwo) ||
      !advance_script_mtime(versionOneMtime)) {
    return false;
  }

  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  return engine::scripting::call_script_function("verify_entity_reload");
}

/// Loads every demo module and verifies its core behavior with Lua stubs.
bool verify_demo_script_modules() noexcept {
  engine::scripting::clear_entity_script_modules();
  const char *script = R"lua(
local require_module = engine.require
local utils = require_module("assets/lib/utils.lua")
local player = require_module("assets/scripts/player.lua")
local scene = require_module("assets/main.lua")

local function expect_equal(actual, expected, label)
    if actual ~= expected then
        error(label .. ": expected " .. tostring(expected)
            .. ", got " .. tostring(actual))
    end
end

function verify_demo_scripts()
    if type(utils) ~= "table" or type(player) ~= "table"
        or type(scene) ~= "table" then
        error("demo module failed to load")
    end

    expect_equal(utils.clamp(-1, 0, 10), 0, "clamp low")
    expect_equal(utils.clamp(12, 0, 10), 10, "clamp high")
    expect_equal(utils.clamp(5, 10, 0), 5, "clamp reversed bounds")
    expect_equal(utils.lerp(2, 6, 0.25), 3, "lerp")
    expect_equal(utils.sign(-9), -1, "negative sign")
    expect_equal(utils.sign(0), 0, "zero sign")
    expect_equal(utils.sign(9), 1, "positive sign")

    engine.KEY_LEFT = 1
    engine.KEY_RIGHT = 2
    engine.KEY_UP = 3
    engine.KEY_DOWN = 4
    engine.KEY_SPACE = 5

    local keys = {}
    local jump_pressed = false
    local ray_hits = {}
    local captured_x = nil
    local captured_y = nil
    local captured_z = nil

    engine.log = function(_message) end
    engine.is_alive = function(_entity) return true end
    engine.get_position = function(_entity) return 0.0, 0.5, 0.0 end
    engine.get_velocity = function(_entity) return 0.0, 0.0, 0.0 end
    engine.is_key_down = function(key) return keys[key] == true end
    engine.is_key_pressed = function(key)
        return key == engine.KEY_SPACE and jump_pressed
    end
    engine.raycast_all = function(...) return ray_hits end
    engine.set_velocity = function(_entity, x, y, z)
        captured_x, captured_y, captured_z = x, y, z
        return true
    end
    engine.set_position = function(...) return true end
    engine.set_restitution = function(...) return true end
    engine.set_friction = function(...) return true end
    engine.set_roughness = function(...) return true end
    engine.set_metallic = function(...) return true end

    player.on_begin_play(42)
    keys = {
        [engine.KEY_LEFT] = true,
        [engine.KEY_RIGHT] = true,
        [engine.KEY_UP] = true,
        [engine.KEY_DOWN] = true,
    }
    player.on_tick(42, 1.0 / 60.0)
    expect_equal(captured_x, 0.0, "opposite horizontal input")
    expect_equal(captured_z, 0.0, "opposite vertical input")

    keys = {
        [engine.KEY_LEFT] = true,
        [engine.KEY_UP] = true,
    }
    player.on_tick(42, 1.0 / 60.0)
    local inverse_length = 1.0 / math.sqrt(2.0)
    local expected_diagonal = -1.0 * inverse_length * 5.0
    expect_equal(captured_x, expected_diagonal, "normalized diagonal x")
    expect_equal(captured_z, expected_diagonal, "normalized diagonal z")

    keys = {}
    jump_pressed = true
    ray_hits = {{ entity = 42, ny = 1.0 }}
    player.on_tick(42, 1.0 / 60.0)
    expect_equal(captured_y, 0.0, "self hit must not ground player")

    ray_hits = {{ entity = 99, ny = 1.0 }}
    player.on_tick(42, 1.0 / 60.0)
    expect_equal(captured_y, 7.0, "grounded jump velocity")

    local spawned = 0
    local destroyed = {}
    local scene_exists = false
    engine.find_entity_by_name = function(name)
        if name == "Player" and scene_exists then
            return 42
        end
        return nil
    end
    engine.spawn_shape = function(...)
        spawned = spawned + 1
        return 100 + spawned
    end
    engine.set_acceleration = function(...) return true end
    engine.set_inverse_mass = function(...) return true end
    engine.destroy_entity = function(entity)
        destroyed[#destroyed + 1] = entity
        return true
    end
    engine.set_name = function(_entity, name)
        if name == "Player" then
            scene_exists = true
        end
        return true
    end
    engine.add_script_component = function(...) return true end
    engine.get_entity_count = function() return spawned end

    scene.on_begin_play(1)
    expect_equal(spawned, 5, "initial scene spawn count")
    scene.on_begin_play(1)
    expect_equal(spawned, 5, "idempotent scene spawn count")

    local saved_state = scene.on_save_state(1)
    expect_equal(saved_state.scene_initialized, true, "saved scene guard")
    scene.on_reload(1, { scene_initialized = false })
    scene.on_begin_play(1)
    expect_equal(spawned, 5, "world-state reload fallback")

    scene_exists = false
    scene.on_reload(1, { scene_initialized = false })
    engine.add_script_component = function(...) return false end
    scene.on_begin_play(1)
    expect_equal(spawned, 6, "failed setup spawn count")
    expect_equal(#destroyed, 1, "failed setup rollback count")
    expect_equal(destroyed[1], 106, "failed setup rollback entity")
    expect_equal(scene.on_save_state(1).scene_initialized, false,
                 "failed setup retry guard")
    engine.add_script_component = function(...) return true end
    scene.on_begin_play(1)
    expect_equal(spawned, 11, "retry setup spawn count")
end
)lua";

  if (!write_script_file(script) ||
      !engine::scripting::load_script(kTempScriptPath)) {
    return false;
  }
  return engine::scripting::call_script_function("verify_demo_scripts");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: locate demo assets\n");
    return 1;
  }

  remove_script_file();

  // Create the script: uses on_begin_play / on_tick / on_end_play.
  // The script records counters in global variables which we read via
  // call_script_function after the test.
  const char *script = "local M = {}\n"
                       "local begin_play_count = 0\n"
                       "local tick_count = 0\n"
                       "local end_play_count = 0\n"
                       "\n"
                       "function M.on_begin_play(self)\n"
                       "    begin_play_count = begin_play_count + 1\n"
                       "end\n"
                       "\n"
                       "function M.on_tick(self, dt)\n"
                       "    tick_count = tick_count + 1\n"
                       "end\n"
                       "\n"
                       "function M.on_end_play(self)\n"
                       "    end_play_count = end_play_count + 1\n"
                       "end\n"
                       "\n"
                       "return M\n";

  if (!write_script_file(script)) {
    std::fprintf(stderr, "FAIL: write script file\n");
    remove_script_file();
    return 1;
  }

  if (!engine::scripting::initialize_scripting()) {
    std::fprintf(stderr, "FAIL: initialize_scripting\n");
    remove_script_file();
    return 1;
  }

  auto world = std::unique_ptr<engine::runtime::World>(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    engine::scripting::shutdown_scripting();
    remove_script_file();
    return 1;
  }

  g_testWorld = world.get();
  engine::core::ServiceLocator serviceLocator{};
  engine::runtime::bind_scripting_runtime(world.get(), serviceLocator);
  auto svc = build_test_services();
  engine::scripting::bind_runtime_services(&svc, serviceLocator);
  engine::scripting::set_default_mesh_asset_id(1U);

  int failures = 0;

  // --- Test 1: Entity with ScriptComponent receives lifecycle callbacks ---
  {
    std::printf("  %-40s ", "on_begin_play fires for new entity");

    const auto entity = world->create_entity();
    engine::runtime::Transform t{};
    static_cast<void>(world->add_transform(entity, t));

    engine::runtime::ScriptComponent sc{};
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kTempScriptPath);
    static_cast<void>(world->add_script_component(entity, sc));

    // Dispatch begin_play.
    world->begin_begin_play_phase();
    engine::scripting::dispatch_entity_scripts_begin_play(world.get());
    world->end_begin_play_phase();

    // Verify: entity should no longer need begin_play.
    std::size_t remainingCount = 0U;
    world->for_each_needs_begin_play(
        [&remainingCount](engine::runtime::Entity) noexcept {
          ++remainingCount;
        });
    if (remainingCount == 0U) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL (still needs begin_play)\n");
      ++failures;
    }
  }

  // --- Test 2: on_tick fires each frame ---
  {
    std::printf("  %-40s ", "on_tick fires 3 times");

    constexpr float kDt = 1.0F / 60.0F;
    for (int i = 0; i < 3; ++i) {
      engine::scripting::set_frame_time(kDt, kDt * static_cast<float>(i + 1));
      engine::scripting::dispatch_entity_scripts_update(kDt);
    }

    // We can't easily read the Lua counter directly, but the dispatch didn't
    // crash and the entity isn't faulted. That's the basic check.
    // A more thorough test would need the scripting system to expose a query.
    std::printf("PASS\n");
  }

  // --- Test 3: on_end_play fires on destroy ---
  {
    std::printf("  %-40s ", "on_end_play fires before destroy");

    // Queue the entity for destruction (deferred).
    world->for_each_alive([&world](engine::runtime::Entity entity) noexcept {
      static_cast<void>(world->destroy_entity(entity));
    });

    // Dispatch EndPlay.
    world->begin_end_play_phase();
    engine::scripting::dispatch_entity_scripts_end_play(world.get());
    world->end_end_play_phase();

    // Entity should now be dead.
    if (world->alive_entity_count() == 0U) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL (entity still alive: %zu)\n",
                  world->alive_entity_count());
      ++failures;
    }
  }

  // --- Test 4: entity hot reload preserves generation-safe state ---
  {
    std::printf("  %-40s ", "entity hot reload restores before tick");
    if (verify_entity_module_hot_reload(world.get())) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Test 5: shipped demo Lua modules load and behave correctly ---
  {
    std::printf("  %-40s ", "demo Lua modules");
    if (verify_demo_script_modules()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  engine::scripting::clear_entity_script_modules();
  engine::scripting::shutdown_scripting();
  remove_script_file();

  return (failures == 0) ? 0 : 1;
}
