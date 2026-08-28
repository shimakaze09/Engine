// Implements Lua scene management bindings for the Engine scripting system.

#include "scene_bindings.h"

#include "binding_util.h"
#include "runtime_binding.h"

#include "engine/core/logging.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::scripting {
namespace {

SceneOp g_pendingSceneOp = SceneOp::None;
char g_pendingScenePath[kPendingScenePathCapacity] = {};
/// Re-entrancy guard for the #198 outgoing-scene on_end_play dispatch: set
/// only while dispatch_entity_scripts_end_for_transition() is running.
bool g_teardownDispatchActive = false;

/// Saves the current world to a scene file from Lua.
int lua_engine_save_scene(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isstring(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if ((path == nullptr) || !script_path_in_jail(path, "save_scene")) {
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
  // #198: a handler running inside the outgoing scene's own on_end_play
  // dispatch cannot be allowed to overwrite the pending op the dispatch was
  // launched to service — reject and warn rather than corrupt it.
  if (g_teardownDispatchActive) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "engine.load_scene ignored: called from on_end_play "
                      "during a scene transition");
    return 0;
  }
  if (!lua_isstring(state, 1)) {
    return 0;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    return 0;
  }
  if (!script_path_in_jail(path, "load_scene") ||
      !copy_path_strict(g_pendingScenePath, sizeof(g_pendingScenePath), path,
                        "load_scene")) {
    return 0;
  }
  g_pendingSceneOp = SceneOp::Load;
  return 0;
}

/// Defers a new-scene request until the runtime can safely process it.
int lua_engine_new_scene(lua_State *state) noexcept {
  static_cast<void>(state);
  // #198: see lua_engine_load_scene — same reentrancy rejection.
  if (g_teardownDispatchActive) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "engine.new_scene ignored: called from on_end_play "
                      "during a scene transition");
    return 0;
  }
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

bool request_scene_load(const char *path) noexcept {
  if (g_teardownDispatchActive || (path == nullptr)) {
    return false;
  }
  if (!script_path_in_jail(path, "load_scene") ||
      !copy_path_strict(g_pendingScenePath, sizeof(g_pendingScenePath), path,
                        "load_scene")) {
    return false;
  }
  g_pendingSceneOp = SceneOp::Load;
  return true;
}

void reset_scene_bindings() noexcept {
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
  g_teardownDispatchActive = false;
}

PendingSceneOpCheckpoint capture_pending_scene_op() noexcept {
  PendingSceneOpCheckpoint checkpoint{};
  checkpoint.op = g_pendingSceneOp;
  std::memcpy(checkpoint.path, g_pendingScenePath, sizeof(checkpoint.path));
  return checkpoint;
}

void restore_pending_scene_op(
    const PendingSceneOpCheckpoint &checkpoint) noexcept {
  g_pendingSceneOp = checkpoint.op;
  std::memcpy(g_pendingScenePath, checkpoint.path, sizeof(g_pendingScenePath));
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

void begin_scene_teardown_dispatch() noexcept {
  g_teardownDispatchActive = true;
}

void end_scene_teardown_dispatch() noexcept {
  g_teardownDispatchActive = false;
}

} // namespace engine::scripting
