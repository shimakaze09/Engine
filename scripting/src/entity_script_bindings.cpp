// Implements private Lua entity script module cache bindings.

#include "entity_script_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/runtime/world.h"
#include "runtime_binding.h"

namespace engine::scripting {
namespace {

/// Stores one cached Lua module table for entity script dispatch.
struct EntityScriptModule final {
  char path[128] = {};
  int registryRef = LUA_NOREF;
  std::int64_t mtime = 0;
  bool reloaded = false;
};

constexpr std::size_t kMaxEntityScriptModules = 32U;
constexpr std::size_t kMaxFaultedEntities = ENGINE_MAX_ENTITIES + 1U;
constexpr std::size_t kMaxModuleLoadDepth = 32U;
constexpr std::size_t kInvalidModuleSlot = kMaxEntityScriptModules;

/// Owns one per-entity table captured while replacing a Lua module.
struct EntitySavedState final {
  core::Entity owner = core::kInvalidEntity;
  std::size_t moduleSlot = kInvalidModuleSlot;
  int registryRef = LUA_NOREF;
};

lua_State *g_state = nullptr;
EntityScriptBindingCallbacks g_callbacks{};
EntityScriptModule g_entityScriptModules[kMaxEntityScriptModules]{};
std::size_t g_entityScriptModuleCount = 0U;
bool g_hasPendingEntityReloads = false;
core::Entity g_entityFaulted[kMaxFaultedEntities]{};
EntitySavedState g_entitySavedState[kMaxFaultedEntities]{};
char g_moduleLoadStack[kMaxModuleLoadDepth][128]{};
std::size_t g_moduleLoadDepth = 0U;

/// Returns the file modification timestamp from the configured callback.
std::int64_t file_mtime(const char *path) noexcept {
  return (g_callbacks.fileMtime != nullptr) ? g_callbacks.fileMtime(path) : 0;
}

/// Logs the current Lua stack error through the configured callback.
void log_lua_error(const char *context) noexcept {
  if (g_callbacks.logLuaError != nullptr) {
    g_callbacks.logLuaError(context);
  } else if (g_state != nullptr) {
    lua_pop(g_state, 1);
  }
}

/// Refreshes Lua hook state through the configured callback.
void refresh_lua_hook() noexcept {
  if (g_callbacks.refreshLuaHook != nullptr) {
    g_callbacks.refreshLuaHook();
  }
}

/// Pushes an entity handle through the configured callback.
void push_entity_handle(lua_State *state, core::Entity entity) noexcept {
  if (g_callbacks.pushEntityHandle != nullptr) {
    g_callbacks.pushEntityHandle(state, entity);
  } else {
    lua_pushnil(state);
  }
}

/// Returns whether this exact entity generation has faulted.
bool entity_is_faulted(core::Entity entity) noexcept {
  return (entity.index > 0U) && (entity.index < kMaxFaultedEntities) &&
         (g_entityFaulted[entity.index] == entity);
}

/// Records a script fault against the exact entity generation.
void mark_entity_faulted(core::Entity entity) noexcept {
  if ((entity.index > 0U) && (entity.index < kMaxFaultedEntities)) {
    g_entityFaulted[entity.index] = entity;
  }
}

/// Clears any script fault stored in this entity index slot.
void clear_entity_fault(core::Entity entity) noexcept {
  if ((entity.index > 0U) && (entity.index < kMaxFaultedEntities)) {
    g_entityFaulted[entity.index] = core::kInvalidEntity;
  }
}

/// Releases one saved-state registry reference and resets its owner metadata.
void release_entity_saved_state(EntitySavedState &savedState) noexcept {
  if ((g_state != nullptr) && (savedState.registryRef != LUA_NOREF)) {
    luaL_unref(g_state, LUA_REGISTRYINDEX, savedState.registryRef);
  }
  savedState = EntitySavedState{};
}

/// Releases all saved-state registry references for entity script hot reload.
void clear_entity_saved_state() noexcept {
  for (EntitySavedState &savedState : g_entitySavedState) {
    release_entity_saved_state(savedState);
  }
}

/// Releases saved-state references captured for one cached module slot.
void clear_entity_saved_state_for_module(std::size_t moduleSlot) noexcept {
  for (EntitySavedState &savedState : g_entitySavedState) {
    if (savedState.moduleSlot == moduleSlot) {
      release_entity_saved_state(savedState);
    }
  }
}

/// Returns true when the requested module path is already loading.
bool module_is_currently_loading(const char *path) noexcept {
  if (path == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < g_moduleLoadDepth; ++i) {
    if (std::strcmp(g_moduleLoadStack[i], path) == 0) {
      return true;
    }
  }
  return false;
}

/// Captures state from every live entity using one cached module.
void capture_entity_saved_state(std::size_t moduleSlot,
                                const EntityScriptModule &mod) noexcept {
  clear_entity_saved_state_for_module(moduleSlot);
  if ((g_state == nullptr) || (runtime_binding().world == nullptr) ||
      (mod.registryRef == LUA_NOREF)) {
    return;
  }

  runtime_binding().world->for_each<runtime::ScriptComponent>(
      [moduleSlot, &mod](runtime::Entity entity,
                         const runtime::ScriptComponent &sc) noexcept {
        if ((std::strcmp(sc.scriptPath, mod.path) != 0) ||
            (entity.index == 0U) || (entity.index >= kMaxFaultedEntities)) {
          return;
        }

        lua_rawgeti(g_state, LUA_REGISTRYINDEX, mod.registryRef);
        if (lua_istable(g_state, -1) == 0) {
          lua_pop(g_state, 1);
          return;
        }

        lua_getfield(g_state, -1, "on_save_state");
        if (lua_isfunction(g_state, -1) == 0) {
          lua_pop(g_state, 2);
          return;
        }

        lua_remove(g_state, -2);
        push_entity_handle(g_state, entity);
        refresh_lua_hook();
        if (lua_pcall(g_state, 1, 1, 0) != LUA_OK) {
          log_lua_error("on_save_state");
          return;
        }

        if (lua_istable(g_state, -1) == 0) {
          lua_pop(g_state, 1);
          return;
        }

        EntitySavedState &savedState = g_entitySavedState[entity.index];
        release_entity_saved_state(savedState);
        savedState.owner = entity;
        savedState.moduleSlot = moduleSlot;
        savedState.registryRef = luaL_ref(g_state, LUA_REGISTRYINDEX);
      });
}

/// Loads a Lua module table, reusing or hot-reloading cache entries.
int get_or_load_entity_script_module(const char *path) noexcept {
  if ((g_state == nullptr) || (path == nullptr) || (path[0] == '\0')) {
    return LUA_NOREF;
  }

  if (module_is_currently_loading(path)) {
    char msg[256] = {};
    std::snprintf(msg, sizeof(msg), "circular module dependency detected: %s",
                  path);
    core::log_message(core::LogLevel::Error, "scripting", msg);
    return LUA_NOREF;
  }

  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    if (std::strcmp(g_entityScriptModules[i].path, path) == 0) {
      EntityScriptModule &mod = g_entityScriptModules[i];
      const std::int64_t currentMtime = file_mtime(path);
      if ((currentMtime != 0) && (mod.mtime != 0) &&
          (currentMtime != mod.mtime)) {
        if (luaL_loadfile(g_state, path) != LUA_OK) {
          log_lua_error("reload entity script");
          return mod.registryRef;
        }

        capture_entity_saved_state(i, mod);
        refresh_lua_hook();

        if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
          log_lua_error("reload entity script");
          clear_entity_saved_state_for_module(i);
          return mod.registryRef;
        }

        if (lua_istable(g_state, -1) == 0) {
          core::log_message(core::LogLevel::Error, "scripting",
                            "entity script must return a module table");
          lua_pop(g_state, 1);
          clear_entity_saved_state_for_module(i);
          return mod.registryRef;
        }

        const int newRef = luaL_ref(g_state, LUA_REGISTRYINDEX);
        if (mod.registryRef != LUA_NOREF) {
          luaL_unref(g_state, LUA_REGISTRYINDEX, mod.registryRef);
        }
        mod.registryRef = newRef;
        mod.mtime = currentMtime;
        mod.reloaded = true;
        g_hasPendingEntityReloads = true;

        char logBuf[256] = {};
        std::snprintf(logBuf, sizeof(logBuf), "hot-reloaded entity script: %s",
                      path);
        core::log_message(core::LogLevel::Info, "scripting", logBuf);
      }

      return mod.registryRef;
    }
  }

  if (g_entityScriptModuleCount >= kMaxEntityScriptModules) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "entity script module limit reached");
    return LUA_NOREF;
  }

  if (g_moduleLoadDepth >= kMaxModuleLoadDepth) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "module load stack overflow");
    return LUA_NOREF;
  }
  std::snprintf(g_moduleLoadStack[g_moduleLoadDepth],
                sizeof(g_moduleLoadStack[g_moduleLoadDepth]), "%s", path);
  ++g_moduleLoadDepth;

  if (luaL_loadfile(g_state, path) != LUA_OK) {
    log_lua_error("load entity script");
    --g_moduleLoadDepth;
    return LUA_NOREF;
  }

  refresh_lua_hook();

  if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
    log_lua_error("exec entity script");
    --g_moduleLoadDepth;
    return LUA_NOREF;
  }

  if (lua_istable(g_state, -1) == 0) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "entity script must return a module table");
    lua_pop(g_state, 1);
    --g_moduleLoadDepth;
    return LUA_NOREF;
  }

  const int ref = luaL_ref(g_state, LUA_REGISTRYINDEX);
  EntityScriptModule &mod = g_entityScriptModules[g_entityScriptModuleCount];
  const std::size_t maxPath = sizeof(mod.path) - 1U;
  const std::size_t pathLen = std::strlen(path);
  const std::size_t copyLen = (pathLen > maxPath) ? maxPath : pathLen;
  std::memcpy(mod.path, path, copyLen);
  mod.path[copyLen] = '\0';
  mod.registryRef = ref;
  mod.mtime = file_mtime(path);
  mod.reloaded = false;
  ++g_entityScriptModuleCount;

  char logBuf[256] = {};
  std::snprintf(logBuf, sizeof(logBuf), "loaded entity script: %s", path);
  core::log_message(core::LogLevel::Info, "scripting", logBuf);
  --g_moduleLoadDepth;
  return ref;
}

/// Calls an entity module function with optional fallback and delta time.
bool call_module_function(int moduleRef, const char *funcName,
                          const char *fallbackName, runtime::Entity entity,
                          bool hasDt, float dt) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return false;
  }

  lua_rawgeti(g_state, LUA_REGISTRYINDEX, moduleRef);
  if (lua_istable(g_state, -1) == 0) {
    lua_pop(g_state, 1);
    return false;
  }

  lua_getfield(g_state, -1, funcName);
  if (lua_isfunction(g_state, -1) == 0) {
    lua_pop(g_state, 1);
    if (fallbackName != nullptr) {
      lua_getfield(g_state, -1, fallbackName);
      if (lua_isfunction(g_state, -1) == 0) {
        lua_pop(g_state, 2);
        return false;
      }
    } else {
      lua_pop(g_state, 1);
      return false;
    }
  }

  lua_remove(g_state, -2);

  push_entity_handle(g_state, entity);
  int nargs = 1;
  if (hasDt) {
    lua_pushnumber(g_state, static_cast<lua_Number>(dt));
    nargs = 2;
  }

  refresh_lua_hook();
  if (lua_pcall(g_state, nargs, 0, 0) != LUA_OK) {
    log_lua_error(funcName);
    mark_entity_faulted(entity);
    return false;
  }
  return true;
}

/// Classifies the result of invoking an optional module reload hook.
enum class ReloadHookResult : std::uint8_t { Missing, Succeeded, Failed };

/// Invokes on_reload with the entity handle and its captured state table.
ReloadHookResult call_module_reload_hook(int moduleRef, runtime::Entity entity,
                                         int savedStateRef) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return ReloadHookResult::Failed;
  }

  lua_rawgeti(g_state, LUA_REGISTRYINDEX, moduleRef);
  if (lua_istable(g_state, -1) == 0) {
    lua_pop(g_state, 1);
    return ReloadHookResult::Failed;
  }

  lua_getfield(g_state, -1, "on_reload");
  if (lua_isfunction(g_state, -1) == 0) {
    lua_pop(g_state, 2);
    return ReloadHookResult::Missing;
  }

  lua_remove(g_state, -2);
  push_entity_handle(g_state, entity);
  if (savedStateRef != LUA_NOREF) {
    lua_rawgeti(g_state, LUA_REGISTRYINDEX, savedStateRef);
  } else {
    lua_pushnil(g_state);
  }

  refresh_lua_hook();
  if (lua_pcall(g_state, 2, 0, 0) != LUA_OK) {
    log_lua_error("on_reload");
    mark_entity_faulted(entity);
    return ReloadHookResult::Failed;
  }
  return ReloadHookResult::Succeeded;
}

/// Delivers pending module reloads before any new-module tick callback.
void dispatch_pending_entity_reloads() noexcept {
  if (!g_hasPendingEntityReloads || (g_state == nullptr) ||
      (runtime_binding().world == nullptr)) {
    return;
  }

  g_hasPendingEntityReloads = false;
  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    EntityScriptModule &module = g_entityScriptModules[i];
    if (!module.reloaded) {
      continue;
    }

    runtime_binding().world->for_each<runtime::ScriptComponent>(
        [i, &module](runtime::Entity entity,
                     const runtime::ScriptComponent &sc) noexcept {
          if (std::strcmp(sc.scriptPath, module.path) != 0) {
            return;
          }

          EntitySavedState *savedState = nullptr;
          int savedStateRef = LUA_NOREF;
          if ((entity.index > 0U) && (entity.index < kMaxFaultedEntities)) {
            EntitySavedState &candidate = g_entitySavedState[entity.index];
            if (candidate.moduleSlot == i) {
              savedState = &candidate;
              if (candidate.owner == entity) {
                savedStateRef = candidate.registryRef;
              }
            }
          }

          clear_entity_fault(entity);
          const ReloadHookResult result = call_module_reload_hook(
              module.registryRef, entity, savedStateRef);
          if (result == ReloadHookResult::Missing) {
            static_cast<void>(call_module_function(module.registryRef,
                                                   "on_begin_play", "on_start",
                                                   entity, false, 0.0F));
          }

          if (savedState != nullptr) {
            release_entity_saved_state(*savedState);
          }
        });

    clear_entity_saved_state_for_module(i);
    module.reloaded = false;
  }
}

} // namespace

void configure_entity_script_bindings(
    lua_State *state, const EntityScriptBindingCallbacks &callbacks) noexcept {
  g_state = state;
  g_callbacks = callbacks;
}

int lua_engine_require(lua_State *state) noexcept {
  const char *path = lua_tostring(state, 1);
  if ((path == nullptr) || (path[0] == '\0')) {
    lua_pushnil(state);
    return 1;
  }
  const int ref = get_or_load_entity_script_module(path);
  if (ref == LUA_NOREF) {
    lua_pushnil(state);
    return 1;
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
  return 1;
}

void dispatch_entity_scripts_start() noexcept {
  if ((g_state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  runtime_binding().world->for_each<runtime::ScriptComponent>(
      [](runtime::Entity entity, const runtime::ScriptComponent &sc) noexcept {
        if (sc.scriptPath[0] == '\0') {
          return;
        }
        if (entity_is_faulted(entity)) {
          return;
        }
        runtime_binding().world->mark_begin_play_done(entity);
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
          return;
        }
        call_module_function(ref, "on_begin_play", "on_start", entity, false,
                             0.0F);
      });
}

void dispatch_entity_scripts_begin_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr)) {
    return;
  }

  world->for_each_needs_begin_play([world](runtime::Entity entity) noexcept {
    world->mark_begin_play_done(entity);
    const auto *sc = world->get_script_component_ptr(entity);
    if ((sc == nullptr) || (sc->scriptPath[0] == '\0')) {
      return;
    }
    if (entity_is_faulted(entity)) {
      return;
    }
    const int ref = get_or_load_entity_script_module(sc->scriptPath);
    if (ref == LUA_NOREF) {
      return;
    }
    call_module_function(ref, "on_begin_play", "on_start", entity, false, 0.0F);
  });
}

void dispatch_entity_scripts_end_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr)) {
    return;
  }

  world->for_each_pending_destroy([world](runtime::Entity entity) noexcept {
    const auto *sc = world->get_script_component_ptr(entity);
    if ((sc == nullptr) || (sc->scriptPath[0] == '\0')) {
      return;
    }
    const int ref = get_or_load_entity_script_module(sc->scriptPath);
    if (ref == LUA_NOREF) {
      return;
    }
    static_cast<void>(call_module_function(ref, "on_end_play", "on_end", entity,
                                           false, 0.0F));
  });
}

void dispatch_entity_scripts_update(float dt) noexcept {
  if ((g_state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  dispatch_pending_entity_reloads();

  runtime_binding().world->for_each<runtime::ScriptComponent>(
      [dt](runtime::Entity entity,
           const runtime::ScriptComponent &sc) noexcept {
        if (sc.scriptPath[0] == '\0') {
          return;
        }
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
          return;
        }
        dispatch_pending_entity_reloads();
        if (entity_is_faulted(entity)) {
          return;
        }
        call_module_function(ref, "on_tick", "on_update", entity, true, dt);
      });
}

void dispatch_entity_scripts_end() noexcept {
  if ((g_state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  runtime_binding().world->for_each<runtime::ScriptComponent>(
      [](runtime::Entity entity, const runtime::ScriptComponent &sc) noexcept {
        if (sc.scriptPath[0] == '\0') {
          return;
        }
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
          return;
        }
        static_cast<void>(call_module_function(ref, "on_end_play", "on_end",
                                               entity, false, 0.0F));
      });
}

void clear_entity_script_modules() noexcept {
  clear_entity_saved_state();
  g_hasPendingEntityReloads = false;
  for (core::Entity &faultedEntity : g_entityFaulted) {
    faultedEntity = core::kInvalidEntity;
  }
  if (g_state != nullptr) {
    for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
      if (g_entityScriptModules[i].registryRef != LUA_NOREF) {
        luaL_unref(g_state, LUA_REGISTRYINDEX,
                   g_entityScriptModules[i].registryRef);
      }
      g_entityScriptModules[i] = EntityScriptModule{};
    }
  } else {
    for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
      g_entityScriptModules[i] = EntityScriptModule{};
    }
  }
  g_entityScriptModuleCount = 0U;
}

void reset_entity_script_bindings() noexcept {
  clear_entity_script_modules();
  g_moduleLoadDepth = 0U;
}

} // namespace engine::scripting
