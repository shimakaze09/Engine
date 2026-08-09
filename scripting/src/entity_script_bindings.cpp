// Implements private Lua entity script module cache bindings.

#include "entity_script_bindings.h"

#include "binding_util.h"
#include "debug_bindings.h"

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
/// lastFailedMtime latches the mtime of a save whose reload failed so the
/// attempt (including its per-entity on_save_state captures) runs once
/// per broken save; the next mtime change retries. registryRef LUA_NOREF
/// marks a never-loaded negative entry whose first-load retries take a
/// small per-file-version attempt budget re-armed by an mtime change.
struct EntityScriptModule final {
  char path[128] = {};
  int registryRef = LUA_NOREF;
  std::int64_t mtime = 0;
  std::int64_t lastFailedMtime = 0;
  std::uint8_t loadAttempts = 0U;
  bool reloaded = false;
};

constexpr std::size_t kMaxEntityScriptModules = 32U;
constexpr std::uint8_t kMaxModuleLoadAttempts = 8U;
constexpr std::size_t kMaxFaultedEntities = ENGINE_MAX_ENTITIES + 1U;
constexpr std::size_t kMaxModuleLoadDepth = 32U;
constexpr std::size_t kInvalidModuleSlot = kMaxEntityScriptModules;
constexpr std::size_t kMaxScriptDispatchEntries = ENGINE_MAX_ENTITIES;
constexpr std::size_t kMaxCaptureDepth = 2U;
constexpr std::size_t kScriptPathSize =
    runtime::ScriptComponent::kMaxPathLength + 1U;

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
bool g_moduleCapacityWarned = false;
bool g_hasPendingEntityReloads = false;
core::Entity g_entityFaulted[kMaxFaultedEntities]{};
EntitySavedState g_entitySavedState[kMaxFaultedEntities]{};
char g_moduleLoadStack[kMaxModuleLoadDepth][128]{};
std::size_t g_moduleLoadDepth = 0U;
int g_endPlayDispatchDepth = 0;
core::Entity g_scriptDispatchOrder[kMaxScriptDispatchEntries]{};
core::Entity g_reloadDispatchOrder[kMaxScriptDispatchEntries]{};
core::Entity g_captureOrder[kMaxCaptureDepth][kMaxScriptDispatchEntries]{};
std::size_t g_captureDepth = 0U;

/// Returns the file modification timestamp from the configured callback.
std::int64_t file_mtime(const char *path) noexcept {
  return (g_callbacks.fileMtime != nullptr) ? g_callbacks.fileMtime(path) : 0;
}

/// Logs the current Lua stack error through the configured callback.
void log_script_error(const char *context) noexcept {
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

/// Snapshots the entities that carry a non-empty script path into the
/// given array, in dense component order, so walk loops survive callbacks
/// that destroy or create scripted entities mid-iteration (swap-and-pop
/// invalidation); entities created after the snapshot are excluded.
std::size_t snapshot_scripted_entities(
    core::Entity (&out)[kMaxScriptDispatchEntries]) noexcept {
  std::size_t count = 0U;
  runtime_binding().world->for_each<runtime::ScriptComponent>(
      [&count, &out](runtime::Entity entity,
                     const runtime::ScriptComponent &sc) noexcept {
        if ((sc.scriptPath[0] == '\0') ||
            (count >= kMaxScriptDispatchEntries)) {
          return;
        }
        out[count] = entity;
        ++count;
      });
  return count;
}

/// Snapshots scripted entities into the tick/start/end dispatch order.
std::size_t snapshot_script_dispatch_order() noexcept {
  return snapshot_scripted_entities(g_scriptDispatchOrder);
}

/// Copies an entity's live script path into caller-owned storage so
/// re-entrant Lua cannot mutate the dense component slot it points into;
/// false when the component is missing or the path is empty.
bool copy_entity_script_path(runtime::World *world, runtime::Entity entity,
                             char (&outPath)[kScriptPathSize]) noexcept {
  const auto *sc = world->get_script_component_ptr(entity);
  if ((sc == nullptr) || (sc->scriptPath[0] == '\0')) {
    return false;
  }
  std::snprintf(outPath, sizeof(outPath), "%s", sc->scriptPath);
  return true;
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

/// Carries one module-function invocation into a protected trampoline.
struct ModuleCallArgs final {
  int moduleRef = LUA_NOREF;
  const char *funcName = nullptr;
  const char *fallbackName = nullptr;
  runtime::Entity entity{};
  bool hasDt = false;
  float dt = 0.0F;
  int savedStateRef = LUA_NOREF;
  bool called = false;
};

/// Protected trampoline: resolves on_save_state on the module table and
/// calls it with the entity handle, returning its single result, so
/// metamethods and allocation failures stay catchable.
int module_save_state_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<ModuleCallArgs *>(lua_touserdata(state, 1));
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->moduleRef);
  if (lua_istable(state, -1) == 0) {
    lua_pushnil(state);
    return 1;
  }
  lua_getfield(state, -1, "on_save_state");
  if (lua_isfunction(state, -1) == 0) {
    lua_pushnil(state);
    return 1;
  }
  args->called = true;
  push_entity_handle(state, args->entity);
  lua_call(state, 1, 1);
  return 1;
}

/// Captures state from every live entity using one cached module. The
/// walk runs over a pre-walk snapshot with per-entity revalidation
/// (alive + path match against a local copy) because on_save_state can
/// destroy scripted entities mid-walk; nested captures (an on_save_state
/// hook requiring another changed module) get their own snapshot buffer
/// up to kMaxCaptureDepth, beyond which capture is skipped with an error.
void capture_entity_saved_state(std::size_t moduleSlot,
                                const EntityScriptModule &mod) noexcept {
  clear_entity_saved_state_for_module(moduleSlot);
  if ((g_state == nullptr) || (runtime_binding().world == nullptr) ||
      (mod.registryRef == LUA_NOREF)) {
    return;
  }
  if (g_captureDepth >= kMaxCaptureDepth) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "save-state capture nested too deep; skipping capture");
    return;
  }

  char modPath[sizeof(mod.path)] = {};
  std::snprintf(modPath, sizeof(modPath), "%.*s",
                static_cast<int>(sizeof(modPath) - 1U), mod.path);
  const int moduleRef = mod.registryRef;

  runtime::World *world = runtime_binding().world;
  core::Entity(&order)[kMaxScriptDispatchEntries] =
      g_captureOrder[g_captureDepth];
  ++g_captureDepth;
  const std::size_t count = snapshot_scripted_entities(order);
  for (std::size_t i = 0U; i < count; ++i) {
    const core::Entity entity = order[i];
    char path[kScriptPathSize] = {};
    if ((entity.index == 0U) || (entity.index >= kMaxFaultedEntities) ||
        !world->is_alive(entity) ||
        !copy_entity_script_path(world, entity, path) ||
        (std::strcmp(path, modPath) != 0)) {
      continue;
    }

    ModuleCallArgs args{};
    args.moduleRef = moduleRef;
    args.entity = entity;
    if (!protected_engine_dispatch(g_state, &module_save_state_trampoline,
                                   &args, 1, "on_save_state")) {
      continue;
    }

    if (lua_istable(g_state, -1) == 0) {
      lua_pop(g_state, 1);
      continue;
    }

    int stateRef = LUA_NOREF;
    if (!protected_registry_ref(g_state, &stateRef,
                                "ref on_save_state result")) {
      continue;
    }
    EntitySavedState &savedState = g_entitySavedState[entity.index];
    release_entity_saved_state(savedState);
    savedState.owner = entity;
    savedState.moduleSlot = moduleSlot;
    savedState.registryRef = stateRef;
  }
  --g_captureDepth;
}

/// Runs one chunk load + exec + registry-ref attempt for a module path.
int attempt_module_load(const char *path) noexcept {
  if (g_moduleLoadDepth >= kMaxModuleLoadDepth) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "module load stack overflow");
    return LUA_NOREF;
  }
  std::snprintf(g_moduleLoadStack[g_moduleLoadDepth],
                sizeof(g_moduleLoadStack[g_moduleLoadDepth]), "%s", path);
  ++g_moduleLoadDepth;

  if (!protected_load_chunk(g_state, path, "load entity script")) {
    --g_moduleLoadDepth;
    return LUA_NOREF;
  }

  refresh_lua_hook();

  if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
    log_script_error("exec entity script");
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

  int ref = LUA_NOREF;
  if (!protected_registry_ref(g_state, &ref, "ref entity script module")) {
    --g_moduleLoadDepth;
    return LUA_NOREF;
  }
  --g_moduleLoadDepth;
  return ref;
}

/// Retries a never-loaded (negative) cache entry within its attempt budget.
int retry_negative_module_entry(EntityScriptModule &mod,
                                const char *path) noexcept {
  const std::int64_t currentMtime = file_mtime(path);
  if (currentMtime != mod.lastFailedMtime) {
    mod.loadAttempts = 0U;
  }
  if (mod.loadAttempts >= kMaxModuleLoadAttempts) {
    return LUA_NOREF;
  }
  ++mod.loadAttempts;
  const int ref = attempt_module_load(path);
  if (ref == LUA_NOREF) {
    mod.lastFailedMtime = currentMtime;
    return LUA_NOREF;
  }
  mod.registryRef = ref;
  mod.mtime = currentMtime;
  mod.lastFailedMtime = 0;
  mod.loadAttempts = 0U;
  mod.reloaded = false;

  char logBuf[256] = {};
  std::snprintf(logBuf, sizeof(logBuf), "loaded entity script: %s", path);
  core::log_message(core::LogLevel::Info, "scripting", logBuf);
  return ref;
}

/// Picks a never-loaded cache entry to evict when the cache is full,
/// preferring one whose retry budget is exhausted; kInvalidModuleSlot when
/// every entry holds a loaded module (loaded modules are never evicted).
std::size_t find_evictable_negative_slot() noexcept {
  std::size_t fallback = kInvalidModuleSlot;
  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    if (g_entityScriptModules[i].registryRef != LUA_NOREF) {
      continue;
    }
    if (g_entityScriptModules[i].loadAttempts >= kMaxModuleLoadAttempts) {
      return i;
    }
    fallback = i;
  }
  return fallback;
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
      if (mod.registryRef == LUA_NOREF) {
        return retry_negative_module_entry(mod, path);
      }
      const std::int64_t currentMtime = file_mtime(path);
      if ((currentMtime != 0) && (mod.mtime != 0) &&
          (currentMtime != mod.mtime) &&
          (currentMtime != mod.lastFailedMtime)) {
        // The reload chunk (and its on_save_state hooks) can require this
        // module again before the new mtime is recorded; the load stack
        // turns that recursion into a logged circular-dependency failure.
        if (g_moduleLoadDepth >= kMaxModuleLoadDepth) {
          core::log_message(core::LogLevel::Error, "scripting",
                            "module load stack overflow");
          return mod.registryRef;
        }
        std::snprintf(g_moduleLoadStack[g_moduleLoadDepth],
                      sizeof(g_moduleLoadStack[g_moduleLoadDepth]), "%s", path);
        ++g_moduleLoadDepth;

        if (!protected_load_chunk(g_state, path, "reload entity script")) {
          mod.lastFailedMtime = currentMtime;
          --g_moduleLoadDepth;
          return mod.registryRef;
        }

        capture_entity_saved_state(i, mod);
        refresh_lua_hook();

        if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
          log_script_error("reload entity script");
          mod.lastFailedMtime = currentMtime;
          clear_entity_saved_state_for_module(i);
          --g_moduleLoadDepth;
          return mod.registryRef;
        }

        if (lua_istable(g_state, -1) == 0) {
          core::log_message(core::LogLevel::Error, "scripting",
                            "entity script must return a module table");
          lua_pop(g_state, 1);
          mod.lastFailedMtime = currentMtime;
          clear_entity_saved_state_for_module(i);
          --g_moduleLoadDepth;
          return mod.registryRef;
        }

        --g_moduleLoadDepth;
        int newRef = LUA_NOREF;
        if (!protected_registry_ref(g_state, &newRef,
                                    "ref entity script module")) {
          mod.lastFailedMtime = currentMtime;
          clear_entity_saved_state_for_module(i);
          return mod.registryRef;
        }
        if (mod.registryRef != LUA_NOREF) {
          luaL_unref(g_state, LUA_REGISTRYINDEX, mod.registryRef);
        }
        mod.registryRef = newRef;
        mod.mtime = currentMtime;
        mod.lastFailedMtime = 0;
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

  std::size_t slot = kInvalidModuleSlot;
  if (g_entityScriptModuleCount < kMaxEntityScriptModules) {
    slot = g_entityScriptModuleCount;
    ++g_entityScriptModuleCount;
  } else {
    slot = find_evictable_negative_slot();
    if (slot == kInvalidModuleSlot) {
      if (!g_moduleCapacityWarned) {
        char msg[256] = {};
        std::snprintf(msg, sizeof(msg),
                      "entity script module cache full (%u loaded): cannot "
                      "load %s; further capacity errors suppressed until the "
                      "cache is cleared",
                      static_cast<unsigned>(kMaxEntityScriptModules), path);
        core::log_message(core::LogLevel::Error, "scripting", msg);
        g_moduleCapacityWarned = true;
      }
      return LUA_NOREF;
    }
    clear_entity_saved_state_for_module(slot);
  }

  EntityScriptModule &mod = g_entityScriptModules[slot];
  mod = EntityScriptModule{};
  const std::size_t maxPath = sizeof(mod.path) - 1U;
  const std::size_t pathLen = std::strlen(path);
  const std::size_t copyLen = (pathLen > maxPath) ? maxPath : pathLen;
  std::memcpy(mod.path, path, copyLen);
  mod.path[copyLen] = '\0';
  mod.lastFailedMtime = -1;
  return retry_negative_module_entry(mod, path);
}

/// Protected trampoline: resolves funcName (or fallbackName) on the module
/// table, pushes the entity handle plus optional dt, and calls it, so
/// metamethods and allocation failures stay catchable.
int module_call_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<ModuleCallArgs *>(lua_touserdata(state, 1));
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->moduleRef);
  if (lua_istable(state, -1) == 0) {
    return 0;
  }
  lua_getfield(state, -1, args->funcName);
  if (lua_isfunction(state, -1) == 0) {
    if (args->fallbackName == nullptr) {
      return 0;
    }
    lua_pop(state, 1);
    lua_getfield(state, -1, args->fallbackName);
    if (lua_isfunction(state, -1) == 0) {
      return 0;
    }
  }
  args->called = true;
  push_entity_handle(state, args->entity);
  int nargs = 1;
  if (args->hasDt) {
    lua_pushnumber(state, static_cast<lua_Number>(args->dt));
    nargs = 2;
  }
  lua_call(state, nargs, 0);
  return 0;
}

/// Calls an entity module function with optional fallback and delta time.
bool call_module_function(int moduleRef, const char *funcName,
                          const char *fallbackName, runtime::Entity entity,
                          bool hasDt, float dt) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return false;
  }

  ModuleCallArgs args{};
  args.moduleRef = moduleRef;
  args.funcName = funcName;
  args.fallbackName = fallbackName;
  args.entity = entity;
  args.hasDt = hasDt;
  args.dt = dt;
  if (!protected_engine_dispatch(g_state, &module_call_trampoline, &args, 0,
                                 funcName)) {
    mark_entity_faulted(entity);
    return false;
  }
  return args.called;
}

/// Classifies the result of invoking an optional module reload hook.
enum class ReloadHookResult : std::uint8_t { Missing, Succeeded, Failed };

/// Protected trampoline: resolves on_reload on the module table and calls
/// it with the entity handle and the captured state table (or nil), so
/// metamethods and allocation failures stay catchable.
int module_reload_trampoline(lua_State *state) noexcept {
  auto *args = static_cast<ModuleCallArgs *>(lua_touserdata(state, 1));
  lua_rawgeti(state, LUA_REGISTRYINDEX, args->moduleRef);
  if (lua_istable(state, -1) == 0) {
    return 0;
  }
  lua_getfield(state, -1, "on_reload");
  if (lua_isfunction(state, -1) == 0) {
    return 0;
  }
  args->called = true;
  push_entity_handle(state, args->entity);
  if (args->savedStateRef != LUA_NOREF) {
    lua_rawgeti(state, LUA_REGISTRYINDEX, args->savedStateRef);
  } else {
    lua_pushnil(state);
  }
  lua_call(state, 2, 0);
  return 0;
}

/// Invokes on_reload with the entity handle and its captured state table.
ReloadHookResult call_module_reload_hook(int moduleRef, runtime::Entity entity,
                                         int savedStateRef) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return ReloadHookResult::Failed;
  }

  ModuleCallArgs args{};
  args.moduleRef = moduleRef;
  args.entity = entity;
  args.savedStateRef = savedStateRef;
  if (!protected_engine_dispatch(g_state, &module_reload_trampoline, &args, 0,
                                 "on_reload")) {
    mark_entity_faulted(entity);
    return ReloadHookResult::Failed;
  }
  return args.called ? ReloadHookResult::Succeeded : ReloadHookResult::Missing;
}

/// Delivers pending module reloads before any new-module tick callback.
/// Each module's delivery walk runs over a pre-walk snapshot with
/// per-entity revalidation (alive + path match against a local copy) so a
/// reload hook that destroys or creates scripted entities mid-walk still
/// delivers to every surviving pre-walk entity exactly once; entities
/// created during the walk are excluded by the snapshot. The walk cannot
/// nest with itself (only C callers reach it), so one buffer suffices.
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

    char modPath[sizeof(module.path)] = {};
    std::snprintf(modPath, sizeof(modPath), "%.*s",
                  static_cast<int>(sizeof(modPath) - 1U), module.path);
    runtime::World *world = runtime_binding().world;
    const std::size_t count = snapshot_scripted_entities(g_reloadDispatchOrder);
    for (std::size_t j = 0U; j < count; ++j) {
      const core::Entity entity = g_reloadDispatchOrder[j];
      char path[kScriptPathSize] = {};
      if (!world->is_alive(entity) ||
          !copy_entity_script_path(world, entity, path) ||
          (std::strcmp(path, modPath) != 0)) {
        continue;
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
      const ReloadHookResult result =
          call_module_reload_hook(module.registryRef, entity, savedStateRef);
      if (result == ReloadHookResult::Missing) {
        static_cast<void>(call_module_function(module.registryRef,
                                               "on_begin_play", "on_start",
                                               entity, false, 0.0F));
      }

      if (savedState != nullptr) {
        release_entity_saved_state(*savedState);
      }
    }

    clear_entity_saved_state_for_module(i);
    module.reloaded = false;
  }
}

/// Fires on_end_play for one entity when it has a script and began play.
void dispatch_entity_end_play(runtime::World *world,
                              runtime::Entity entity) noexcept {
  if (!world->has_begun_play(entity)) {
    return;
  }
  char path[kScriptPathSize] = {};
  if (!copy_entity_script_path(world, entity, path)) {
    return;
  }
  const int ref = get_or_load_entity_script_module(path);
  if (ref == LUA_NOREF) {
    return;
  }
  ++g_endPlayDispatchDepth;
  static_cast<void>(
      call_module_function(ref, "on_end_play", "on_end", entity, false, 0.0F));
  --g_endPlayDispatchDepth;
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

  runtime::World *world = runtime_binding().world;
  const std::size_t count = snapshot_script_dispatch_order();
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = g_scriptDispatchOrder[i];
    char path[kScriptPathSize] = {};
    if (!world->is_alive(entity) ||
        !copy_entity_script_path(world, entity, path) ||
        entity_is_faulted(entity)) {
      continue;
    }
    arm_debug_lua_hook(g_state);
    const int ref = get_or_load_entity_script_module(path);
    if (ref == LUA_NOREF) {
      continue;
    }
    world->mark_begin_play_done(entity);
    call_module_function(ref, "on_begin_play", "on_start", entity, false,
                         0.0F);
  }
}

void dispatch_entity_scripts_begin_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr)) {
    return;
  }

  world->for_each_needs_begin_play([world](runtime::Entity entity) noexcept {
    char path[kScriptPathSize] = {};
    if (!copy_entity_script_path(world, entity, path) ||
        entity_is_faulted(entity)) {
      world->mark_begin_play_done(entity);
      return;
    }
    const int ref = get_or_load_entity_script_module(path);
    if (ref == LUA_NOREF) {
      return;
    }
    world->mark_begin_play_done(entity);
    call_module_function(ref, "on_begin_play", "on_start", entity, false, 0.0F);
  });
}

bool in_end_play_dispatch() noexcept { return g_endPlayDispatchDepth > 0; }

void dispatch_entity_subtree_end_play(runtime::World *world,
                                      runtime::Entity entity) noexcept {
  if ((g_state == nullptr) || (world == nullptr)) {
    return;
  }
  world->for_each_subtree_member(entity,
                                 [world](runtime::Entity member) noexcept {
                                   dispatch_entity_end_play(world, member);
                                 });
}

void dispatch_entity_scripts_end_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr)) {
    return;
  }

  world->for_each_pending_destroy([world](runtime::Entity entity) noexcept {
    dispatch_entity_end_play(world, entity);
  });
}

void dispatch_entity_scripts_update(float dt) noexcept {
  if ((g_state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  dispatch_pending_entity_reloads();

  runtime::World *world = runtime_binding().world;
  const std::size_t count = snapshot_script_dispatch_order();
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = g_scriptDispatchOrder[i];
    char path[kScriptPathSize] = {};
    if (!world->is_alive(entity) ||
        !copy_entity_script_path(world, entity, path)) {
      continue;
    }
    arm_debug_lua_hook(g_state);
    const int ref = get_or_load_entity_script_module(path);
    if (ref == LUA_NOREF) {
      continue;
    }
    dispatch_pending_entity_reloads();
    if (!world->is_alive(entity) || entity_is_faulted(entity)) {
      continue;
    }
    call_module_function(ref, "on_tick", "on_update", entity, true, dt);
  }
}

void dispatch_entity_scripts_end() noexcept {
  if ((g_state == nullptr) || (runtime_binding().world == nullptr)) {
    return;
  }

  runtime::World *world = runtime_binding().world;
  const std::size_t count = snapshot_script_dispatch_order();
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = g_scriptDispatchOrder[i];
    char path[kScriptPathSize] = {};
    if (!world->is_alive(entity) ||
        !copy_entity_script_path(world, entity, path)) {
      continue;
    }
    arm_debug_lua_hook(g_state);
    const int ref = get_or_load_entity_script_module(path);
    if (ref == LUA_NOREF) {
      continue;
    }
    static_cast<void>(call_module_function(ref, "on_end_play", "on_end",
                                           entity, false, 0.0F));
  }
}

void clear_entity_script_modules() noexcept {
  clear_entity_saved_state();
  g_moduleCapacityWarned = false;
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
  g_captureDepth = 0U;
}

} // namespace engine::scripting
