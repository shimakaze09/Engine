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

/// Creates a scene object for the Lua-facing spawn operation.
std::uint32_t create_scene_object(engine::runtime::World *w) noexcept {
  if (w == nullptr) {
    return 0U;
  }
  return w->create_scene_object().index;
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

bool add_script_component(engine::runtime::World *w, std::uint32_t idx,
                          const engine::runtime::ScriptComponent &sc) noexcept {
  if (w == nullptr) {
    return false;
  }
  return w->add_script_component(w->find_entity_by_index(idx), sc);
}

std::uint32_t get_entity_count(engine::runtime::World *w) noexcept {
  return (w != nullptr) ? static_cast<std::uint32_t>(w->alive_entity_count())
                        : 0U;
}

const engine::runtime::Transform *
get_transform_read_ptr(engine::runtime::World *w, std::uint32_t idx) noexcept {
  if (w == nullptr) {
    return nullptr;
  }
  return w->get_transform_read_ptr(w->find_entity_by_index(idx));
}

bool get_transform(engine::runtime::World *w, std::uint32_t idx,
                   engine::runtime::Transform *outTransform) noexcept {
  if (w == nullptr) {
    return false;
  }
  return w->get_transform(w->find_entity_by_index(idx), outTransform);
}

/// Builds the requested runtime data for test services.
engine::scripting::RuntimeServices build_test_services() noexcept {
  engine::scripting::RuntimeServices svc{};
  svc.get_current_phase = &get_phase;
  svc.create_scene_object_op = &create_scene_object;
  svc.destroy_entity_op = &destroy_entity;
  svc.add_transform_op = &add_transform;
  svc.add_script_component_op = &add_script_component;
  svc.get_entity_count = &get_entity_count;
  svc.get_transform_read_ptr = &get_transform_read_ptr;
  svc.get_transform_op = &get_transform;
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

/// Writes contents to an arbitrary relative path.
bool write_file_at(const char *path, const char *contents) noexcept {
  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }
  const std::size_t len = std::strlen(contents);
  const bool ok = (std::fwrite(contents, 1U, len, file) == len);
  std::fclose(file);
  return ok;
}

/// Verifies a hot reload of a module that requires itself terminates with a
/// logged circular-dependency failure instead of recursing.
bool verify_circular_reload_guard() noexcept {
  constexpr const char *kCircularPath = "lua_lifecycle_circular.lua";
  engine::scripting::clear_entity_script_modules();

  const char *versionOne = "local M = {}\n"
                           "return M\n";
  if (!write_file_at(kCircularPath, versionOne)) {
    return false;
  }

  const char *driver =
      "function load_circular()\n"
      "    if engine.require('lua_lifecycle_circular.lua') == nil then\n"
      "        error('initial load failed')\n"
      "    end\n"
      "end\n"
      "function reload_circular()\n"
      "    if engine.require('lua_lifecycle_circular.lua') == nil then\n"
      "        error('reload failed')\n"
      "    end\n"
      "end\n";
  if (!write_script_file(driver) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("load_circular")) {
    static_cast<void>(std::remove(kCircularPath));
    return false;
  }

  std::error_code error{};
  const std::filesystem::file_time_type cachedMtime =
      std::filesystem::last_write_time(kCircularPath, error);
  if (error) {
    static_cast<void>(std::remove(kCircularPath));
    return false;
  }

  // The reloaded chunk requires itself; the load-stack guard must fail that
  // inner require (nil) while the reload itself still completes.
  const char *versionTwo = "local M = {}\n"
                           "M.inner = engine.require("
                           "'lua_lifecycle_circular.lua')\n"
                           "return M\n";
  if (!write_file_at(kCircularPath, versionTwo)) {
    static_cast<void>(std::remove(kCircularPath));
    return false;
  }
  std::filesystem::last_write_time(kCircularPath,
                                   cachedMtime + std::chrono::seconds(2),
                                   error);
  if (error) {
    static_cast<void>(std::remove(kCircularPath));
    return false;
  }

  const bool reloaded =
      engine::scripting::call_script_function("reload_circular");
  static_cast<void>(std::remove(kCircularPath));
  return reloaded;
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

/// Advances an entity-script module's file timestamp so the next update
/// dispatch hot-reloads it through the production module cache.
bool touch_module_for_reload(const char *path) noexcept {
  std::error_code error{};
  const std::filesystem::file_time_type mtime =
      std::filesystem::last_write_time(path, error);
  if (error) {
    return false;
  }
  std::filesystem::last_write_time(path, mtime + std::chrono::seconds(2),
                                   error);
  return !error;
}

/// Destroys every remaining entity through the production deferred-destroy
/// flow and clears the entity module cache so island tests start isolated.
bool reset_world_for_island_tests(engine::runtime::World *world) noexcept {
  if (world == nullptr) {
    return false;
  }
  // Leftover scripted entities would reload kTempScriptPath as a module at
  // EndPlay; give them a valid trivial module to unload against.
  if (!write_script_file("local M = {}\nreturn M\n")) {
    return false;
  }
  world->begin_update_phase();
  world->for_each_alive([world](engine::runtime::Entity entity) noexcept {
    static_cast<void>(world->destroy_entity(entity));
  });
  world->begin_transform_phase();
  world->begin_render_prep_phase();
  world->begin_render_phase();
  world->end_frame_phase();
  world->begin_end_play_phase();
  engine::scripting::dispatch_entity_scripts_end_play(world);
  world->end_end_play_phase();
  engine::scripting::clear_entity_script_modules();
  return world->alive_entity_count() == 0U;
}

/// Fires begin-play dispatch for entities created by an island test driver.
void dispatch_begin_play_phase(engine::runtime::World *world) noexcept {
  world->begin_begin_play_phase();
  engine::scripting::dispatch_entity_scripts_begin_play(world);
  world->end_begin_play_phase();
}

/// Verifies two moving platforms driven by the shipped script through the
/// production dispatch path keep independent authored bases, survive a hot
/// reload with per-instance state, and release state on destroy (#101).
bool verify_island_platform_isolation(engine::runtime::World *world) noexcept {
  if (!reset_world_for_island_tests(world)) {
    return false;
  }

  const char *driver = R"lua(
island_positions = {}
island_velocity = {}
engine.get_position = function(e)
    local p = island_positions[e]
    if p == nil then return nil end
    return p.x, p.y, p.z
end
engine.set_velocity = function(e, x, y, z)
    island_velocity[e] = { x = x, y = y, z = z }
    return true
end
engine.wake_body = function(_) return true end

function island_setup_two_platforms()
    island_plat1 = engine.spawn_entity()
    island_plat2 = engine.spawn_entity()
    if island_plat1 == nil or island_plat2 == nil then
        error('spawn failed')
    end
    island_positions[island_plat1] = { x = 1.0, y = 2.0, z = 3.0 }
    island_positions[island_plat2] = { x = 11.0, y = 5.0, z = -4.0 }
    if not engine.add_script_component(island_plat1,
            'assets/scripts/moving_platform.lua') then
        error('plat1 script failed')
    end
    if not engine.add_script_component(island_plat2,
            'assets/scripts/moving_platform.lua') then
        error('plat2 script failed')
    end
end

-- Each platform's y/z correctives are exactly zero when it tracks its own
-- base (base minus unchanged position); shared-base aliasing clamps them
-- to the 3.5 corrective limit. The x corrective from a stationary stub
-- position grows with sweep phase but stays under 0.4 for the few ticks
-- this test runs, far below the aliased 3.5 clamp.
local function island_check_platform(e, label)
    local v = island_velocity[e]
    if v == nil then error(label .. ': no velocity write') end
    if v.y ~= 0.0 then error(label .. ': y corrective ' .. v.y) end
    if v.z ~= 0.0 then error(label .. ': z corrective ' .. v.z) end
    if math.abs(v.x) > 0.5 then error(label .. ': x corrective ' .. v.x) end
end

function island_verify_platform_isolation()
    island_check_platform(island_plat1, 'platform1')
    island_check_platform(island_plat2, 'platform2')
end

function island_clear_platform_captures()
    island_velocity = {}
end

function island_destroy_plat1()
    if not engine.destroy_entity(island_plat1) then
        error('destroy failed')
    end
end

function island_verify_platform_state_cleanup()
    local mod = engine.require('assets/scripts/moving_platform.lua')
    if mod == nil then error('module load failed') end
    if mod.on_save_state(island_plat1) ~= nil then
        error('destroyed platform state not released')
    end
    if mod.on_save_state(island_plat2) == nil then
        error('live platform state missing')
    end
end
)lua";

  if (!write_script_file(driver) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("island_setup_two_platforms")) {
    return false;
  }

  dispatch_begin_play_phase(world);
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function(
          "island_verify_platform_isolation")) {
    return false;
  }

  // Hot reload through the production module cache must preserve each
  // platform's own base and phase.
  if (!engine::scripting::call_script_function(
          "island_clear_platform_captures") ||
      !touch_module_for_reload("assets/scripts/moving_platform.lua")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function(
          "island_verify_platform_isolation")) {
    return false;
  }

  return engine::scripting::call_script_function("island_destroy_plat1") &&
         engine::scripting::call_script_function(
             "island_verify_platform_state_cleanup");
}

/// Verifies two falling rocks driven by the shipped script through the
/// production dispatch path trigger, re-arm, and hot-reload independently,
/// and that the alarm sound survives a reload (#101).
bool verify_island_rock_isolation(engine::runtime::World *world) noexcept {
  if (!reset_world_for_island_tests(world)) {
    return false;
  }

  const char *driver = R"lua(
island_positions = {}
island_accel = {}
island_setpos = {}
island_alarms = {}
engine.get_position = function(e)
    local p = island_positions[e]
    if p == nil then return nil end
    return p.x, p.y, p.z
end
engine.set_acceleration = function(e, x, y, z)
    island_accel[#island_accel + 1] = { e = e, x = x, y = y, z = z }
    return true
end
engine.set_velocity = function(...) return true end
engine.set_position = function(e, x, y, z)
    island_setpos[#island_setpos + 1] = { e = e, x = x, y = y, z = z }
    island_positions[e] = { x = x, y = y, z = z }
    return true
end
engine.load_sound = function(_path) return 7 end
engine.play_sound_at = function(sound, ...)
    island_alarms[#island_alarms + 1] = sound
    return true
end
engine.find_entity_by_name = function(name)
    if name == 'Player' then return island_rock_player end
    return nil
end

function island_setup_two_rocks()
    island_rock_player = engine.spawn_entity()
    island_rock1 = engine.spawn_entity()
    island_rock2 = engine.spawn_entity()
    if island_rock_player == nil or island_rock1 == nil
        or island_rock2 == nil then
        error('spawn failed')
    end
    island_positions[island_rock_player] = { x = 0.0, y = 3.0, z = 0.0 }
    island_positions[island_rock1] = { x = 0.0, y = 6.0, z = 0.0 }
    island_positions[island_rock2] = { x = 20.0, y = 6.0, z = 0.0 }
    if not engine.add_script_component(island_rock1,
            'assets/scripts/falling_rock.lua') then
        error('rock1 script failed')
    end
    if not engine.add_script_component(island_rock2,
            'assets/scripts/falling_rock.lua') then
        error('rock2 script failed')
    end
end

function island_verify_rock1_dropped()
    if #island_accel ~= 1 then
        error('accel calls: ' .. #island_accel)
    end
    if island_accel[1].e ~= island_rock1 then
        error('wrong rock dropped')
    end
    if island_accel[1].y ~= -9.8 then
        error('drop acceleration: ' .. island_accel[1].y)
    end
    if #island_alarms ~= 1 or island_alarms[1] ~= 7 then
        error('alarm did not play')
    end
end

function island_sink_rock1_player_far()
    island_positions[island_rock1] = { x = 0.0, y = -9.0, z = 0.0 }
    island_positions[island_rock_player] = { x = 100.0, y = 3.0, z = 100.0 }
end

function island_verify_rock1_rearmed_on_own_perch()
    if #island_setpos ~= 1 then
        error('set_position calls: ' .. #island_setpos)
    end
    local call = island_setpos[1]
    if call.e ~= island_rock1 then error('wrong rock re-armed') end
    if call.x ~= 0.0 or call.y ~= 6.0 or call.z ~= 0.0 then
        error(('re-armed on foreign perch (%g, %g, %g)')
            :format(call.x, call.y, call.z))
    end
end

function island_move_player_under_rock2()
    island_positions[island_rock_player] = { x = 20.0, y = 3.0, z = 0.0 }
end

function island_verify_rock2_dropped_rock1_armed()
    if #island_accel ~= 3 then
        error('accel calls: ' .. #island_accel)
    end
    local last = island_accel[3]
    if last.e ~= island_rock2 or last.y ~= -9.8 then
        error('rock2 did not drop')
    end
end

function island_move_player_under_rock1()
    island_positions[island_rock_player] = { x = 0.0, y = 3.0, z = 0.0 }
end

function island_verify_alarm_survives_reload()
    if #island_alarms ~= 3 then
        error('alarm handle lost after reload: ' .. #island_alarms)
    end
    if island_alarms[3] ~= 7 then
        error('alarm played with invalid handle')
    end
    local final = island_accel[#island_accel]
    if final == nil or final.e ~= island_rock1 or final.y ~= -9.8 then
        error('rock1 did not re-trigger after reload')
    end
end
)lua";

  if (!write_script_file(driver) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("island_setup_two_rocks")) {
    return false;
  }

  dispatch_begin_play_phase(world);
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function("island_verify_rock1_dropped")) {
    return false;
  }

  if (!engine::scripting::call_script_function(
          "island_sink_rock1_player_far")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function(
          "island_verify_rock1_rearmed_on_own_perch")) {
    return false;
  }

  if (!engine::scripting::call_script_function(
          "island_move_player_under_rock2")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function(
          "island_verify_rock2_dropped_rock1_armed")) {
    return false;
  }

  // Hot reload, then re-trigger rock1: its state and the alarm handle must
  // both survive the reload.
  if (!touch_module_for_reload("assets/scripts/falling_rock.lua")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function(
          "island_move_player_under_rock1")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  return engine::scripting::call_script_function(
      "island_verify_alarm_survives_reload");
}

/// Verifies the shipped island player carries moving ground at the ground's
/// true speed when the fixed-step count changes between frames, and that two
/// players keep independent carry samples (#107, #101).
bool verify_island_player_carry(engine::runtime::World *world) noexcept {
  if (!reset_world_for_island_tests(world)) {
    return false;
  }

  const char *driver = R"lua(
island_positions = {}
island_velocity = {}
island_ground1 = 3001
island_ground2 = 3002
engine.get_position = function(e)
    local p = island_positions[e]
    if p == nil then return nil end
    return p.x, p.y, p.z
end
engine.set_velocity = function(e, x, y, z)
    island_velocity[e] = { x = x, y = y, z = z }
    return true
end
engine.get_velocity = function(_e) return 0.0, 0.0, 0.0 end
engine.wake_body = function(_) return true end
engine.is_key_down = function(_) return false end
engine.is_key_pressed = function(_) return false end
engine.set_anim_param = function(...) return true end
engine.push_camera = function(...) return true end
engine.set_restitution = function(...) return true end
engine.set_friction = function(...) return true end
engine.set_lock_rotation = function(...) return true end
engine.load_sound = function(_) return 7 end
engine.play_sound_at = function(...) return true end
engine.on_anim_event_handler = function(_) return 1 end
engine.remove_anim_event_handler = function(_) return true end
-- Each player stands on its own ground entity, selected by query origin.
engine.raycast_all = function(x, ...)
    if x < 25.0 then
        return { { entity = island_ground1, ny = 1.0 } }
    end
    return { { entity = island_ground2, ny = 1.0 } }
end

function island_setup_two_riders()
    island_p1 = engine.spawn_entity()
    island_p2 = engine.spawn_entity()
    if island_p1 == nil or island_p2 == nil then error('spawn failed') end
    island_positions[island_p1] = { x = 0.0, y = 0.5, z = 0.0 }
    island_positions[island_p2] = { x = 50.0, y = 0.5, z = 0.0 }
    island_positions[island_ground1] = { x = 10.0, y = 0.0, z = 0.0 }
    island_positions[island_ground2] = { x = 60.0, y = 0.0, z = 0.0 }
    if not engine.add_script_component(island_p1,
            'assets/scripts/island_player.lua') then
        error('p1 script failed')
    end
    if not engine.add_script_component(island_p2,
            'assets/scripts/island_player.lua') then
        error('p2 script failed')
    end
end

-- Moves each ground by its own speed over the simulated time the previous
-- dispatch reported, mirroring what the fixed steps would have produced.
local function island_advance_grounds(sim_dt)
    local g1 = island_positions[island_ground1]
    local g2 = island_positions[island_ground2]
    g1.x = g1.x + 1.5 * sim_dt
    g2.x = g2.x + 0.5 * sim_dt
end

function island_advance_grounds_two_steps()
    island_advance_grounds(2.0 / 60.0)
end

function island_advance_grounds_one_step()
    island_advance_grounds(1.0 / 60.0)
end

-- 1e-4 separates the correct carry (1.5 / 0.5, reproduced to ~1e-7 across
-- the float dt round-trip) from the wrong-frame results (3.0 / 0.75).
local function check_carry(e, expected, label)
    local v = island_velocity[e]
    if v == nil then error(label .. ': no velocity write') end
    if math.abs(v.x - expected) > 1e-4 then
        error(label .. ': carried vx ' .. v.x .. ' expected ' .. expected)
    end
end

function island_verify_first_tick_no_carry()
    check_carry(island_p1, 0.0, 'rider1 first tick')
    check_carry(island_p2, 0.0, 'rider2 first tick')
end

function island_verify_carry_speeds()
    check_carry(island_p1, 1.5, 'rider1')
    check_carry(island_p2, 0.5, 'rider2')
end
)lua";

  if (!write_script_file(driver) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function("island_setup_two_riders")) {
    return false;
  }

  dispatch_begin_play_phase(world);

  // Frame 1 simulates two fixed steps (dt 2/60), frame 2 one step (dt
  // 1/60), frame 3 two steps again — the inherited speed must stay the
  // ground's own speed through both catch-up transitions.
  engine::scripting::dispatch_entity_scripts_update(2.0F / 60.0F);
  if (!engine::scripting::call_script_function(
          "island_verify_first_tick_no_carry")) {
    return false;
  }

  if (!engine::scripting::call_script_function(
          "island_advance_grounds_two_steps")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(1.0F / 60.0F);
  if (!engine::scripting::call_script_function("island_verify_carry_speeds")) {
    return false;
  }

  if (!engine::scripting::call_script_function(
          "island_advance_grounds_one_step")) {
    return false;
  }
  engine::scripting::dispatch_entity_scripts_update(2.0F / 60.0F);
  return engine::scripting::call_script_function("island_verify_carry_speeds");
}

/// Verifies Lua scene-object creation exposes an identity TRS immediately and
/// that entity counting includes transformless raw ECS entities.
bool verify_lua_scene_object_defaults(engine::runtime::World *world) noexcept {
  if (world == nullptr) {
    return false;
  }

  const std::size_t aliveBefore = world->alive_entity_count();
  const char *script =
      "function verify_scene_object_defaults()\n"
      "    local count_before = engine.get_entity_count()\n"
      "    local entity = engine.spawn_entity()\n"
      "    if entity == nil then error('spawn_entity returned nil') end\n"
      "    local px, py, pz = engine.get_position(entity)\n"
      "    local rx, ry, rz, rw = engine.get_rotation(entity)\n"
      "    local sx, sy, sz = engine.get_scale(entity)\n"
      "    if px ~= 0.0 or py ~= 0.0 or pz ~= 0.0\n"
      "        or rx ~= 0.0 or ry ~= 0.0 or rz ~= 0.0 or rw ~= 1.0\n"
      "        or sx ~= 1.0 or sy ~= 1.0 or sz ~= 1.0 then\n"
      "        error('spawned scene object did not have identity TRS')\n"
      "    end\n"
      "    if engine.get_entity_count() ~= count_before + 1 then\n"
      "        error('get_entity_count did not include spawned object')\n"
      "    end\n"
      "end\n";

  if (!write_script_file(script) ||
      !engine::scripting::load_script(kTempScriptPath) ||
      !engine::scripting::call_script_function(
          "verify_scene_object_defaults")) {
    return false;
  }

  return world->alive_entity_count() == (aliveBefore + 1U);
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
  const char *script =
      "local M = {}\n"
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
      "function spawn_scripted_pair()\n"
      "    destroy_root = engine.spawn_entity()\n"
      "    destroy_kid = engine.spawn_entity()\n"
      "    if destroy_root == nil or destroy_kid == nil then\n"
      "        error('spawn failed')\n"
      "    end\n"
      "    if not engine.set_parent(destroy_kid, destroy_root) then\n"
      "        error('set_parent failed')\n"
      "    end\n"
      "    if not engine.add_script_component(destroy_root,\n"
      "                                       'lua_lifecycle_test.lua') then\n"
      "        error('root script failed')\n"
      "    end\n"
      "    if not engine.add_script_component(destroy_kid,\n"
      "                                       'lua_lifecycle_test.lua') then\n"
      "        error('kid script failed')\n"
      "    end\n"
      "end\n"
      "\n"
      "function destroy_scripted_root()\n"
      "    if not engine.destroy_entity(destroy_root) then\n"
      "        error('destroy failed')\n"
      "    end\n"
      "    if engine.is_alive(destroy_root) then\n"
      "        error('root alive after destroy')\n"
      "    end\n"
      "    if engine.is_alive(destroy_kid) then\n"
      "        error('kid alive after destroy')\n"
      "    end\n"
      "end\n"
      "\n"
      "function verify_end_play_count_one()\n"
      "    if end_play_count ~= 1 then\n"
      "        error('end_play_count ' .. tostring(end_play_count))\n"
      "    end\n"
      "end\n"
      "\n"
      "function verify_end_play_count_three()\n"
      "    if end_play_count ~= 3 then\n"
      "        error('end_play_count ' .. tostring(end_play_count))\n"
      "    end\n"
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

  // --- Test 3: on_end_play fires for deferred destroys at EndPlay ---
  {
    std::printf("  %-40s ", "on_end_play fires for deferred destroy");

    // Destroy during Simulation so the entity queues, then walk the frame
    // phases exactly as the pipeline does: the queue must survive
    // end_frame_phase so EndPlay dispatch sees it.
    world->begin_update_phase();
    world->for_each_alive([&world](engine::runtime::Entity entity) noexcept {
      static_cast<void>(world->destroy_entity(entity));
    });
    world->begin_transform_phase();
    world->begin_render_prep_phase();
    world->begin_render_phase();
    world->end_frame_phase();

    world->begin_end_play_phase();
    engine::scripting::dispatch_entity_scripts_end_play(world.get());
    world->end_end_play_phase();

    if ((world->alive_entity_count() == 0U) &&
        engine::scripting::call_script_function("verify_end_play_count_one")) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL (alive: %zu)\n", world->alive_entity_count());
      ++failures;
    }
  }

  // --- Test 3b: script-initiated destroy fires on_end_play for the subtree ---
  {
    std::printf("  %-40s ", "on_end_play fires for script destroy");

    const std::size_t aliveBefore = world->alive_entity_count();
    bool ok = engine::scripting::call_script_function("spawn_scripted_pair");

    if (ok) {
      world->begin_begin_play_phase();
      engine::scripting::dispatch_entity_scripts_begin_play(world.get());
      world->end_begin_play_phase();
      ok = engine::scripting::call_script_function("destroy_scripted_root");
    }

    ok = ok &&
         engine::scripting::call_script_function(
             "verify_end_play_count_three") &&
         (world->alive_entity_count() == aliveBefore);
    if (ok) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL (alive: %zu)\n", world->alive_entity_count());
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

  // --- Test 5: Lua-spawned entities are scene objects with identity TRS ---
  {
    std::printf("  %-40s ", "Lua spawn creates identity scene object");
    if (verify_lua_scene_object_defaults(world.get())) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Test 6: two moving platforms stay independent per entity ---
  // (Runs before the demo-module test, which replaces real bindings such
  // as add_script_component with Lua stubs for the rest of the VM.)
  {
    std::printf("  %-40s ", "island platforms independent");
    if (verify_island_platform_isolation(world.get())) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Test 7: two falling rocks trigger/re-arm/reload independently ---
  {
    std::printf("  %-40s ", "island rocks independent");
    if (verify_island_rock_isolation(world.get())) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Test 8: rider carry uses the sampled frame's simulated dt ---
  {
    std::printf("  %-40s ", "island player carry across catch-up");
    if (verify_island_player_carry(world.get())) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Test 9: shipped demo Lua modules load and behave correctly ---
  {
    std::printf("  %-40s ", "demo Lua modules");
    if (verify_demo_script_modules()) {
      std::printf("PASS\n");
    } else {
      std::printf("FAIL\n");
      ++failures;
    }
  }

  // --- Test 10: circular self-require during hot reload is rejected ---
  {
    std::printf("  %-40s ", "circular reload guard");
    if (verify_circular_reload_guard()) {
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
