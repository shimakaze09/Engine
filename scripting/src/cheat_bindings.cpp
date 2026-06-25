// Implements Lua and console cheat bindings for the Engine scripting system.

#include "cheat_bindings.h"

#include "game_bindings.h"
#include "runtime_binding.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "engine/core/console.h"
#include "engine/runtime/world.h"

namespace engine::scripting {
namespace {

bool g_godModeEnabled = false;
bool g_noclipEnabled = false;

/// Returns whether god mode is enabled to Lua.
int lua_engine_is_god_mode(lua_State *state) noexcept {
  lua_pushboolean(state, g_godModeEnabled ? 1 : 0);
  return 1;
}

/// Returns whether noclip is enabled to Lua.
int lua_engine_is_noclip(lua_State *state) noexcept {
  lua_pushboolean(state, g_noclipEnabled ? 1 : 0);
  return 1;
}

/// Toggles god mode from the console.
void cmd_god(const char *const * /*args*/, int /*argCount*/,
             void * /*userData*/) noexcept {
  g_godModeEnabled = !g_godModeEnabled;
  core::console_print(g_godModeEnabled ? "God mode ON" : "God mode OFF");
}

/// Toggles noclip from the console.
void cmd_noclip(const char *const * /*args*/, int /*argCount*/,
                void * /*userData*/) noexcept {
  g_noclipEnabled = !g_noclipEnabled;
  core::console_print(g_noclipEnabled ? "Noclip ON" : "Noclip OFF");
}

/// Spawns a prefab from the console.
void cmd_spawn(const char *const *args, int argCount,
               void * /*userData*/) noexcept {
  if (argCount < 2) {
    core::console_print("Usage: spawn <prefab> [x y z]");
    return;
  }
  if ((runtime_binding().world == nullptr) ||
      (runtime_binding().services == nullptr) ||
      (runtime_binding().services->instantiate_prefab == nullptr)) {
    core::console_print("Cannot spawn: world not ready");
    return;
  }
  const std::uint32_t entityIndex =
      runtime_binding().services->instantiate_prefab(runtime_binding().world,
                                                    args[1]);
  if (entityIndex == 0U) {
    core::console_print("Spawn failed (prefab not found?)");
    return;
  }
  if ((argCount >= 5) &&
      (runtime_binding().services->add_transform_op != nullptr)) {
    runtime::Transform transform{};
    transform.position.x = static_cast<float>(std::atof(args[2]));
    transform.position.y = static_cast<float>(std::atof(args[3]));
    transform.position.z = static_cast<float>(std::atof(args[4]));
    transform.scale = {1.0F, 1.0F, 1.0F};
    transform.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    runtime_binding().services->add_transform_op(
        runtime_binding().world, entityIndex, transform);
  }
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "Spawned entity %u", entityIndex);
  core::console_print(buffer);
}

/// Destroys all non-player entities from the console.
void cmd_kill_all(const char *const * /*args*/, int /*argCount*/,
                  void * /*userData*/) noexcept {
  if ((runtime_binding().world == nullptr) ||
      (runtime_binding().services == nullptr) ||
      (runtime_binding().services->destroy_entity_op == nullptr)) {
    core::console_print("Cannot kill_all: world not ready");
    return;
  }
  std::size_t destroyed = 0U;
  runtime_binding().world->for_each_alive(
      [&destroyed](runtime::Entity entity) noexcept {
        if (is_player_controller_entity(entity)) {
          return;
        }
        runtime_binding().services->destroy_entity_op(runtime_binding().world,
                                                      entity.index);
        ++destroyed;
      });
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "Destroyed %zu entities", destroyed);
  core::console_print(buffer);
}

} // namespace

void register_cheat_status_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_is_god_mode);
  lua_setfield(state, -2, "is_god_mode");
  lua_pushcfunction(state, &lua_engine_is_noclip);
  lua_setfield(state, -2, "is_noclip");
}

void register_cheat_commands() noexcept {
  core::console_register_command("god", cmd_god, nullptr,
                                 "Toggle god mode (invincibility)");
  core::console_register_command("noclip", cmd_noclip, nullptr,
                                 "Toggle noclip (no collision)");
  core::console_register_command("spawn", cmd_spawn, nullptr,
                                 "Spawn a prefab: spawn <path> [x y z]");
  core::console_register_command("kill_all", cmd_kill_all, nullptr,
                                 "Destroy all entities except player");
}

void reset_cheat_bindings() noexcept {
  g_godModeEnabled = false;
  g_noclipEnabled = false;
}

bool bindable_is_god_mode() noexcept { return g_godModeEnabled; }

bool bindable_is_noclip() noexcept { return g_noclipEnabled; }

} // namespace engine::scripting
