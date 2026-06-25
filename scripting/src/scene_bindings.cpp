// Implements Lua scene management bindings for the Engine scripting system.

#include "scene_bindings.h"

#include "runtime_binding.h"

#include <cstdint>
#include <cstdio>

namespace engine::scripting {
namespace {

/// Identifies the deferred scene operation requested from Lua.
enum class SceneOp : std::uint8_t { None, Load, New };

SceneOp g_pendingSceneOp = SceneOp::None;
char g_pendingScenePath[512] = {};

/// Saves the current world to a scene file from Lua.
int lua_engine_save_scene(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isstring(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok =
      (runtime_binding().services != nullptr) &&
              (runtime_binding().services->save_scene != nullptr)
          ? runtime_binding().services->save_scene(runtime_binding().world,
                                                   path)
          : false;
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Defers a scene load request until the runtime can safely process it.
int lua_engine_load_scene(lua_State *state) noexcept {
  if (!lua_isstring(state, 1)) {
    return 0;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    return 0;
  }
  std::snprintf(g_pendingScenePath, sizeof(g_pendingScenePath), "%s", path);
  g_pendingScenePath[sizeof(g_pendingScenePath) - 1U] = '\0';
  g_pendingSceneOp = SceneOp::Load;
  return 0;
}

/// Defers a new-scene request until the runtime can safely process it.
int lua_engine_new_scene(lua_State *state) noexcept {
  static_cast<void>(state);
  g_pendingSceneOp = SceneOp::New;
  return 0;
}

} // namespace

void register_scene_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_save_scene);
  lua_setfield(state, -2, "save_scene");
  lua_pushcfunction(state, &lua_engine_load_scene);
  lua_setfield(state, -2, "load_scene");
  lua_pushcfunction(state, &lua_engine_new_scene);
  lua_setfield(state, -2, "new_scene");
}

void reset_scene_bindings() noexcept {
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
}

bool has_pending_scene_op() noexcept {
  return g_pendingSceneOp != SceneOp::None;
}

bool pending_scene_op_is_load() noexcept {
  return g_pendingSceneOp == SceneOp::Load;
}

bool pending_scene_op_is_new() noexcept {
  return g_pendingSceneOp == SceneOp::New;
}

const char *get_pending_scene_path() noexcept { return g_pendingScenePath; }

void clear_pending_scene_op() noexcept { reset_scene_bindings(); }

} // namespace engine::scripting
