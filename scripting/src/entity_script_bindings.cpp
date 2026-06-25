// Implements private Lua entity script module cache bindings.

#include "entity_script_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
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

lua_State *g_state = nullptr;
EntityScriptBindingCallbacks g_callbacks{};
EntityScriptModule g_entityScriptModules[kMaxEntityScriptModules]{};
std::size_t g_entityScriptModuleCount = 0U;
bool g_entityFaulted[kMaxFaultedEntities]{};
int g_entitySavedState[kMaxFaultedEntities]{};
bool g_entitySavedStateInit = false;
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

/// Initializes saved-state registry refs on first use.
void init_entity_saved_state() noexcept {
  if (!g_entitySavedStateInit) {
    for (auto &ref : g_entitySavedState) {
      ref = LUA_NOREF;
    }
    g_entitySavedStateInit = true;
  }
}

/// Releases saved-state registry refs for entity script hot reload.
void clear_entity_saved_state() noexcept {
  if (g_state == nullptr) {
    return;
  }
  for (auto &ref : g_entitySavedState) {
    if (ref != LUA_NOREF) {
      luaL_unref(g_state, LUA_REGISTRYINDEX, ref);
      ref = LUA_NOREF;
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
        init_entity_saved_state();
        if ((runtime_binding().world != nullptr) &&
            (mod.registryRef != LUA_NOREF)) {
          runtime_binding().world->for_each<runtime::ScriptComponent>(
              [&mod](runtime::Entity entity,
                     const runtime::ScriptComponent &sc) noexcept {
                if (std::strcmp(sc.scriptPath, mod.path) != 0) {
                  return;
                }
                if (entity.index >= kMaxFaultedEntities) {
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
                lua_pushinteger(g_state,
                                static_cast<lua_Integer>(entity.index));
                refresh_lua_hook();
                if (lua_pcall(g_state, 1, 1, 0) != LUA_OK) {
                  log_lua_error("on_save_state");
                  return;
                }
                if (lua_istable(g_state, -1) != 0) {
                  if (g_entitySavedState[entity.index] != LUA_NOREF) {
                    luaL_unref(g_state, LUA_REGISTRYINDEX,
                               g_entitySavedState[entity.index]);
                  }
                  g_entitySavedState[entity.index] =
                      luaL_ref(g_state, LUA_REGISTRYINDEX);
                } else {
                  lua_pop(g_state, 1);
                }
              });
        }

        if (luaL_loadfile(g_state, path) != LUA_OK) {
          log_lua_error("reload entity script");
          return mod.registryRef;
        }

        refresh_lua_hook();

        if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
          log_lua_error("reload entity script");
          return mod.registryRef;
        }

        if (lua_istable(g_state, -1) == 0) {
          core::log_message(core::LogLevel::Error, "scripting",
                            "entity script must return a module table");
          lua_pop(g_state, 1);
          return mod.registryRef;
        }

        const int newRef = luaL_ref(g_state, LUA_REGISTRYINDEX);
        if (mod.registryRef != LUA_NOREF) {
          luaL_unref(g_state, LUA_REGISTRYINDEX, mod.registryRef);
        }
        mod.registryRef = newRef;
        mod.mtime = currentMtime;
        mod.reloaded = true;

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
    if ((entity.index > 0U) && (entity.index < kMaxFaultedEntities)) {
      g_entityFaulted[entity.index] = true;
    }
    return false;
  }
  return true;
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
        if ((entity.index < kMaxFaultedEntities) &&
            g_entityFaulted[entity.index]) {
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
    if ((entity.index < kMaxFaultedEntities) && g_entityFaulted[entity.index]) {
      return;
    }
    const int ref = get_or_load_entity_script_module(sc->scriptPath);
    if (ref == LUA_NOREF) {
      return;
    }
    call_module_function(ref, "on_begin_play", "on_start", entity, false,
                         0.0F);
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
    static_cast<void>(call_module_function(ref, "on_end_play", "on_end",
                                           entity, false, 0.0F));
  });
}

void dispatch_entity_scripts_update(float dt) noexcept {
  if ((g_state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    if (!g_entityScriptModules[i].reloaded) {
      continue;
    }

    const char *reloadedPath = g_entityScriptModules[i].path;
    const int moduleRef = g_entityScriptModules[i].registryRef;
    runtime_binding().world->for_each<runtime::ScriptComponent>(
        [reloadedPath, moduleRef](runtime::Entity entity,
                                  const runtime::ScriptComponent &sc) noexcept {
          if (std::strcmp(sc.scriptPath, reloadedPath) != 0) {
            return;
          }
          if (entity.index < kMaxFaultedEntities) {
            g_entityFaulted[entity.index] = false;
          }

          static_cast<void>(call_module_function(
              moduleRef, "on_reload", nullptr, entity, false, 0.0F));
          static_cast<void>(call_module_function(moduleRef, "on_begin_play",
                                                 "on_start", entity, false,
                                                 0.0F));
        });
    g_entityScriptModules[i].reloaded = false;
  }

  runtime_binding().world->for_each<runtime::ScriptComponent>(
      [dt](runtime::Entity entity,
           const runtime::ScriptComponent &sc) noexcept {
        if (sc.scriptPath[0] == '\0') {
          return;
        }
        if ((entity.index < kMaxFaultedEntities) &&
            g_entityFaulted[entity.index]) {
          return;
        }
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
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
  clear_entity_saved_state();
  g_moduleLoadDepth = 0U;
  for (std::size_t i = 0U; i < kMaxFaultedEntities; ++i) {
    g_entityFaulted[i] = false;
  }
}

} // namespace engine::scripting
