// Implements private Lua entity script module cache bindings.

#include "entity_script_bindings.h"

#include "binding_util.h"
#include "debug_bindings.h"
#include "physics_bindings.h"
#include "scene_bindings.h"
#include "script_reload.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/thread_affinity.h"
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
  // Dispatch frame (g_modulePollSerial) whose timestamp poll this entry
  // already took; the next entity sharing the module reuses the answer.
  std::uint64_t polledSerial = 0U;
};

// Distinct script files (entity scripts and engine.require libraries) one
// run can hold. Unity, Godot and Unreal bound script types only by memory;
// a fixed table keeps this allocation-free, so it is sized past what a
// project of this engine's scale reaches (an entry is under 200 bytes),
// and lookups go through a hash index rather than a scan.
constexpr std::size_t kMaxEntityScriptModules = 1024U;
// Open-addressed path index: slot + 1 per bucket, 0 for empty. Twice the
// table, a power of two, so a probe ends quickly.
constexpr std::size_t kModuleIndexBuckets = 2U * kMaxEntityScriptModules;
// The first refused paths are each reported once; one more line says the
// rest go unlisted, so a project far past the limit never logs per frame.
constexpr std::size_t kMaxReportedRefusals = 64U;
constexpr std::uint8_t kMaxModuleLoadAttempts = 8U;
constexpr std::size_t kMaxFaultedEntities = ENGINE_MAX_ENTITIES + 1U;
constexpr std::size_t kMaxModuleLoadDepth = 32U;
constexpr std::size_t kInvalidModuleSlot = kMaxEntityScriptModules;
constexpr std::size_t kMaxScriptDispatchEntries = ENGINE_MAX_ENTITIES;
constexpr std::size_t kMaxCaptureDepth = 2U;
/// Begin-play passes one dispatch runs: each pass delivers to the entities
/// the previous one spawned, so a spawn chain deeper than this finishes on
/// the next frame's dispatch.
constexpr std::size_t kMaxBeginPlayPasses = 8U;
constexpr std::size_t kScriptPathSize =
    math::ScriptBehaviour::kMaxPathLength + 1U;
constexpr std::size_t kMaxBehaviours = math::kMaxScriptBehaviours;

/// Set once the behaviour's on_begin_play has been called: it then ticks,
/// and receives on_end_play when its entity ends play.
constexpr std::uint8_t kBehaviourBegun = 1U;
/// Set when one of the behaviour's hooks failed; it receives nothing more
/// until its script reloads.
constexpr std::uint8_t kBehaviourFaulted = 2U;

/// The run-time state of one behaviour instance. A script appears at most
/// once in an entity's list, so the module it runs names the instance and
/// the state survives the list being reordered.
struct BehaviourState final {
  /// Module table slot + 1; 0 marks an unused entry.
  std::uint16_t moduleKey = 0U;
  std::uint8_t flags = 0U;
  /// The table on_save_state returned while its module was replaced, held
  /// until on_reload receives it.
  int savedStateRef = LUA_NOREF;
};

/// The behaviour states of one entity generation, indexed by entity index.
struct EntityBehaviourStates final {
  core::Entity owner = core::kInvalidEntity;
  BehaviourState entries[kMaxBehaviours]{};
};

/// One entity's behaviour list, copied before any hook runs so a callback
/// that edits the list changes the next dispatch, not this one.
struct BehaviourSnapshot final {
  std::size_t count = 0U;
  char paths[kMaxBehaviours][kScriptPathSize] = {};
  bool enabled[kMaxBehaviours] = {};
};

lua_State *g_state = nullptr;
EntityScriptBindingCallbacks g_callbacks{};
EntityScriptModule g_entityScriptModules[kMaxEntityScriptModules]{};
std::size_t g_entityScriptModuleCount = 0U;
// Advances once per dispatch pass (update, start, begin/end play) so a
// module's file is polled at most once per pass, not once per scripted
// entity; a later pass in the same frame polls again, which keeps
// a script that appears between passes visible to the next one.
std::uint64_t g_modulePollSerial = 1U;
std::uint64_t g_mtimePolls = 0U;
std::uint16_t g_moduleIndex[kModuleIndexBuckets]{};
std::uint64_t g_reportedRefusals[kMaxReportedRefusals]{};
std::size_t g_reportedRefusalCount = 0U;
bool g_hasPendingEntityReloads = false;
EntityBehaviourStates g_behaviourStates[kMaxFaultedEntities]{};
// The behaviour whose hook is running: its entity and module slot.
core::Entity g_hookEntity = core::kInvalidEntity;
std::size_t g_hookModuleSlot = kInvalidModuleSlot;
char g_moduleLoadStack[kMaxModuleLoadDepth][128]{};
std::size_t g_moduleLoadDepth = 0U;
int g_endPlayDispatchDepth = 0;
core::Entity g_scriptDispatchOrder[kMaxScriptDispatchEntries]{};
core::Entity g_reloadDispatchOrder[kMaxScriptDispatchEntries]{};
core::Entity g_beginPlayOrder[kMaxScriptDispatchEntries]{};
// Begin-play dispatch that last attempted each entity index, so one dispatch
// attempts an entity once however many passes it runs: an entity whose
// module failed to load stays pending for the next frame's retry.
std::uint32_t g_beginPlayAttempt[kMaxFaultedEntities]{};
std::uint32_t g_beginPlayDispatch = 0U;
core::Entity g_captureOrder[kMaxCaptureDepth][kMaxScriptDispatchEntries]{};
std::size_t g_captureDepth = 0U;

/// Returns the file modification timestamp from the configured callback.
std::int64_t file_mtime(const char *path) noexcept {
  ++g_mtimePolls;
  return (g_callbacks.fileMtime != nullptr) ? g_callbacks.fileMtime(path) : 0;
}

/// The table slot holding `path`, or kInvalidModuleSlot.
std::size_t find_module_slot(const char *path) noexcept {
  std::size_t bucket = static_cast<std::size_t>(core::fnv1a_64(path)) &
                       (kModuleIndexBuckets - 1U);
  for (std::size_t probe = 0U; probe < kModuleIndexBuckets; ++probe) {
    const std::uint16_t entry = g_moduleIndex[bucket];
    if (entry == 0U) {
      return kInvalidModuleSlot;
    }
    const std::size_t slot = static_cast<std::size_t>(entry) - 1U;
    if (std::strcmp(g_entityScriptModules[slot].path, path) == 0) {
      return slot;
    }
    bucket = (bucket + 1U) & (kModuleIndexBuckets - 1U);
  }
  return kInvalidModuleSlot;
}

/// Rebuilds the path index from the table. Runs only when a module enters
/// or leaves the table, never per dispatch.
void rebuild_module_index() noexcept {
  for (std::uint16_t &entry : g_moduleIndex) {
    entry = 0U;
  }
  for (std::size_t slot = 0U; slot < g_entityScriptModuleCount; ++slot) {
    std::size_t bucket = static_cast<std::size_t>(
                             core::fnv1a_64(g_entityScriptModules[slot].path)) &
                         (kModuleIndexBuckets - 1U);
    while (g_moduleIndex[bucket] != 0U) {
      bucket = (bucket + 1U) & (kModuleIndexBuckets - 1U);
    }
    g_moduleIndex[bucket] = static_cast<std::uint16_t>(slot + 1U);
  }
}

/// Logs a full table's refusal of `path` once per path, so each script
/// that could not load is named rather than only the first.
void report_module_refusal(const char *path) noexcept {
  const std::uint64_t hash = core::fnv1a_64(path);
  const std::size_t recorded = (g_reportedRefusalCount < kMaxReportedRefusals)
                                   ? g_reportedRefusalCount
                                   : kMaxReportedRefusals;
  for (std::size_t i = 0U; i < recorded; ++i) {
    if (g_reportedRefusals[i] == hash) {
      return;
    }
  }
  if (g_reportedRefusalCount > kMaxReportedRefusals) {
    return;
  }
  char msg[256] = {};
  if (g_reportedRefusalCount == kMaxReportedRefusals) {
    // One last line, so a project far past the limit is not logged per
    // frame for every refused path.
    std::snprintf(msg, sizeof(msg),
                  "entity script module table full (%u modules loaded): "
                  "cannot load %s; further refused paths are not listed",
                  static_cast<unsigned>(kMaxEntityScriptModules), path);
  } else {
    g_reportedRefusals[g_reportedRefusalCount] = hash;
    std::snprintf(msg, sizeof(msg),
                  "entity script module table full (%u modules loaded): "
                  "cannot load %s",
                  static_cast<unsigned>(kMaxEntityScriptModules), path);
  }
  ++g_reportedRefusalCount;
  core::log_message(core::LogLevel::Error, "scripting", msg);
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
/// Bridge visitor state for snapshot_scripted_entities.
struct ScriptedSnapshot final {
  core::Entity *out = nullptr;
  std::size_t count = 0U;
};

/// Records one scripted entity into the snapshot while capacity remains.
void snapshot_scripted_visit(core::Entity entity,
                             const runtime::ScriptComponent &sc,
                             void *context) noexcept {
  auto *snapshot = static_cast<ScriptedSnapshot *>(context);
  if ((math::script_behaviour_count(sc) == 0U) ||
      (snapshot->count >= kMaxScriptDispatchEntries)) {
    return;
  }
  snapshot->out[snapshot->count] = entity;
  ++snapshot->count;
}

std::size_t snapshot_scripted_entities(
    core::Entity (&out)[kMaxScriptDispatchEntries]) noexcept {
  if (!runtime_bound()) {
    return 0U;
  }
  ScriptedSnapshot snapshot{};
  snapshot.out = out;
  runtime_binding().services->for_each_scripted_entity(
      runtime_binding().world, &snapshot_scripted_visit, &snapshot);
  return snapshot.count;
}

/// Snapshots scripted entities into the tick/start/end dispatch order.
std::size_t snapshot_script_dispatch_order() noexcept {
  return snapshot_scripted_entities(g_scriptDispatchOrder);
}

/// Copies an entity's behaviour list into caller-owned storage so
/// re-entrant Lua cannot change the dense component slot it points into;
/// false when the entity has no behaviour.
bool snapshot_entity_behaviours(runtime::World *world, runtime::Entity entity,
                                BehaviourSnapshot *out) noexcept {
  out->count = 0U;
  if (!runtime_bound() ||
      (runtime_binding().services->find_script_component_op == nullptr)) {
    return false;
  }
  const runtime::ScriptComponent *sc =
      runtime_binding().services->find_script_component_op(world, entity);
  if (sc == nullptr) {
    return false;
  }
  const std::size_t count = math::script_behaviour_count(*sc);
  for (std::size_t i = 0U; i < count; ++i) {
    std::memcpy(out->paths[i], sc->behaviours[i].scriptPath, kScriptPathSize);
    out->enabled[i] = sc->behaviours[i].enabled;
  }
  out->count = count;
  return count > 0U;
}

/// True when `entity` is live in `world`; the bridge answers.
bool world_entity_alive(runtime::World *world,
                        runtime::Entity entity) noexcept {
  return runtime_bound() && runtime_binding().services->is_alive(world, entity);
}

/// Releases one saved-state registry reference.
void release_saved_state(BehaviourState &state) noexcept {
  if ((g_state != nullptr) && (state.savedStateRef != LUA_NOREF)) {
    luaL_unref(g_state, LUA_REGISTRYINDEX, state.savedStateRef);
  }
  state.savedStateRef = LUA_NOREF;
}

/// The behaviour states of this exact entity generation, or nullptr. With
/// `claim`, an index slot still holding an older generation's states is
/// cleared and taken.
EntityBehaviourStates *entity_states(core::Entity entity, bool claim) noexcept {
  if ((entity.index == 0U) || (entity.index >= kMaxFaultedEntities)) {
    return nullptr;
  }
  EntityBehaviourStates &states = g_behaviourStates[entity.index];
  if (states.owner == entity) {
    return &states;
  }
  if (!claim) {
    return nullptr;
  }
  for (BehaviourState &state : states.entries) {
    release_saved_state(state);
    state = BehaviourState{};
  }
  states.owner = entity;
  return &states;
}

/// The state of the behaviour running module `moduleSlot` on `entity`, or
/// nullptr when it has none yet.
BehaviourState *find_behaviour_state(core::Entity entity,
                                     std::size_t moduleSlot) noexcept {
  EntityBehaviourStates *states = entity_states(entity, false);
  if ((states == nullptr) || (moduleSlot >= kMaxEntityScriptModules)) {
    return nullptr;
  }
  for (BehaviourState &state : states->entries) {
    if (state.moduleKey == moduleSlot + 1U) {
      return &state;
    }
  }
  return nullptr;
}

/// The state of the behaviour running module `moduleSlot` on `entity`,
/// creating it when absent. When every entry is taken, one whose module
/// the entity no longer lists (a removed or replaced behaviour) is
/// reused; nullptr only for an invalid entity or slot.
BehaviourState *claim_behaviour_state(core::Entity entity,
                                      std::size_t moduleSlot,
                                      const BehaviourSnapshot &list) noexcept {
  if (BehaviourState *found = find_behaviour_state(entity, moduleSlot)) {
    return found;
  }
  EntityBehaviourStates *states = entity_states(entity, true);
  if ((states == nullptr) || (moduleSlot >= kMaxEntityScriptModules)) {
    return nullptr;
  }
  BehaviourState *chosen = nullptr;
  for (BehaviourState &state : states->entries) {
    if (state.moduleKey == 0U) {
      chosen = &state;
      break;
    }
  }
  for (std::size_t i = 0U; (chosen == nullptr) && (i < kMaxBehaviours); ++i) {
    BehaviourState &state = states->entries[i];
    const std::size_t stateSlot = state.moduleKey - 1U;
    bool listed = false;
    for (std::size_t b = 0U; b < list.count; ++b) {
      listed = listed || (find_module_slot(list.paths[b]) == stateSlot);
    }
    if (!listed) {
      chosen = &state;
    }
  }
  if (chosen == nullptr) {
    return nullptr;
  }
  release_saved_state(*chosen);
  *chosen = BehaviourState{};
  chosen->moduleKey = static_cast<std::uint16_t>(moduleSlot + 1U);
  return chosen;
}

/// True when the behaviour has the given flag.
bool behaviour_has(core::Entity entity, std::size_t moduleSlot,
                   std::uint8_t flag) noexcept {
  const BehaviourState *state = find_behaviour_state(entity, moduleSlot);
  return (state != nullptr) && ((state->flags & flag) != 0U);
}

/// Records a fault against the behaviour, so it receives nothing more
/// until its script reloads; the entity's other behaviours run on.
void mark_behaviour_faulted(core::Entity entity,
                            std::size_t moduleSlot) noexcept {
  EntityBehaviourStates *states = entity_states(entity, true);
  if ((states == nullptr) || (moduleSlot >= kMaxEntityScriptModules)) {
    return;
  }
  for (BehaviourState &state : states->entries) {
    if (state.moduleKey == moduleSlot + 1U) {
      state.flags |= kBehaviourFaulted;
      return;
    }
  }
  for (BehaviourState &state : states->entries) {
    if (state.moduleKey == 0U) {
      state.moduleKey = static_cast<std::uint16_t>(moduleSlot + 1U);
      state.flags = kBehaviourFaulted;
      return;
    }
  }
}

/// Releases the saved-state references captured for one module slot.
void clear_saved_states_for_module(std::size_t moduleSlot) noexcept {
  for (EntityBehaviourStates &states : g_behaviourStates) {
    for (BehaviourState &state : states.entries) {
      if (state.moduleKey == moduleSlot + 1U) {
        release_saved_state(state);
      }
    }
  }
}

/// Forgets every behaviour state, for a module table cleared with the
/// world it served.
void clear_behaviour_states() noexcept {
  for (EntityBehaviourStates &states : g_behaviourStates) {
    for (BehaviourState &state : states.entries) {
      release_saved_state(state);
      state = BehaviourState{};
    }
    states.owner = core::kInvalidEntity;
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
  std::size_t moduleSlot = kInvalidModuleSlot;
  const char *funcName = nullptr;
  const char *fallbackName = nullptr;
  runtime::Entity entity{};
  bool hasDt = false;
  float dt = 0.0F;
  int savedStateRef = LUA_NOREF;
  bool called = false;
};

/// Runs `trampoline` as a protected engine dispatch with the behaviour it
/// calls recorded as the running hook, so engine.get_property(self, ...)
/// reads that behaviour's values. The previous hook is restored after, as
/// one hook may run another entity's through an engine call.
bool protected_hook_dispatch(lua_CFunction trampoline, ModuleCallArgs *args,
                             int results, const char *context) noexcept {
  const core::Entity previousEntity = g_hookEntity;
  const std::size_t previousSlot = g_hookModuleSlot;
  g_hookEntity = args->entity;
  g_hookModuleSlot = args->moduleSlot;
  const bool ok =
      protected_engine_dispatch(g_state, trampoline, args, results, context);
  g_hookEntity = previousEntity;
  g_hookModuleSlot = previousSlot;
  return ok;
}

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

/// The reload check an entity module must pass: its chunk returns the
/// module table.
bool returns_module_table(lua_State *state, void *) noexcept {
  if (lua_istable(state, -1) != 0) {
    return true;
  }
  core::log_message(core::LogLevel::Error, "scripting",
                    "entity script must return a module table");
  return false;
}

/// True when `list` runs `modPath` in a behaviour that is enabled or has
/// begun play: the instances a reload saves and restores.
bool behaviour_is_live(core::Entity entity, std::size_t moduleSlot,
                       const char *modPath,
                       const BehaviourSnapshot &list) noexcept {
  for (std::size_t b = 0U; b < list.count; ++b) {
    if (std::strcmp(list.paths[b], modPath) == 0) {
      return list.enabled[b] ||
             behaviour_has(entity, moduleSlot, kBehaviourBegun);
    }
  }
  return false;
}

/// Captures state from every live behaviour (enabled, or begun) that runs
/// one cached module. The walk runs over a pre-walk snapshot with per-entity
/// revalidation (alive + the module still listed, against a local copy)
/// because on_save_state can destroy scripted entities mid-walk; nested
/// captures (an on_save_state hook requiring another changed module) get
/// their own snapshot buffer up to kMaxCaptureDepth, beyond which capture
/// is skipped with an error.
void capture_entity_saved_state(std::size_t moduleSlot,
                                const EntityScriptModule &mod) noexcept {
  clear_saved_states_for_module(moduleSlot);
  if ((g_state == nullptr) || !runtime_bound() ||
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
    BehaviourSnapshot list{};
    if (!world_entity_alive(world, entity) ||
        !snapshot_entity_behaviours(world, entity, &list) ||
        !behaviour_is_live(entity, moduleSlot, modPath, list)) {
      continue;
    }

    ModuleCallArgs args{};
    args.moduleRef = moduleRef;
    args.moduleSlot = moduleSlot;
    args.entity = entity;
    if (!protected_hook_dispatch(&module_save_state_trampoline, &args, 1,
                                 "on_save_state")) {
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
    BehaviourState *state = claim_behaviour_state(entity, moduleSlot, list);
    if (state == nullptr) {
      luaL_unref(g_state, LUA_REGISTRYINDEX, stateRef);
      continue;
    }
    release_saved_state(*state);
    state->savedStateRef = stateRef;
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

  if (!traced_pcall(g_state, 0, 1)) {
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
  mod.polledSerial = g_modulePollSerial;
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

  char stagedPath[sizeof(g_entityScriptModules[0].path)] = {};
  if (!copy_path_strict(stagedPath, sizeof(stagedPath), path,
                        "entity script module load") ||
      !script_path_in_jail(stagedPath, "entity script module load")) {
    return LUA_NOREF;
  }

  if (module_is_currently_loading(path)) {
    char msg[256] = {};
    std::snprintf(msg, sizeof(msg), "circular module dependency detected: %s",
                  path);
    core::log_message(core::LogLevel::Error, "scripting", msg);
    return LUA_NOREF;
  }

  {
    const std::size_t i = find_module_slot(path);
    if (i != kInvalidModuleSlot) {
      EntityScriptModule &mod = g_entityScriptModules[i];
      if (mod.polledSerial == g_modulePollSerial) {
        // Already polled this frame: the answer stands for every entity
        // sharing the module (LUA_NOREF for a negative entry).
        return mod.registryRef;
      }
      mod.polledSerial = g_modulePollSerial;
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

        // The new chunk runs as one reload transaction, like the main
        // script's: its top-level bindings and effects commit only once it
        // has run cleanly and returned a module table, so a broken save
        // leaves nothing behind. The live instances' on_save_state hooks
        // run only after that, when the swap is certain.
        if (!protected_load_chunk(g_state, path, "reload entity script")) {
          mod.lastFailedMtime = currentMtime;
          --g_moduleLoadDepth;
          return mod.registryRef;
        }
        refresh_lua_hook();
        const ReloadOutcome outcome = run_chunk_as_reload(
            g_state, "reload entity script", 1, &returns_module_table, nullptr);
        --g_moduleLoadDepth;
        if (outcome == ReloadOutcome::Busy) {
          // Nothing ran; the next poll tries this save again.
          return mod.registryRef;
        }
        if ((outcome != ReloadOutcome::Committed) &&
            (outcome != ReloadOutcome::CommitFailed)) {
          mod.lastFailedMtime = currentMtime;
          return mod.registryRef;
        }

        // The new module table is on the stack; the old one still answers
        // mod.registryRef while the live instances save their state.
        const int stackTop = lua_gettop(g_state);
        capture_entity_saved_state(i, mod);
        lua_settop(g_state, stackTop);
        int newRef = LUA_NOREF;
        if (!protected_registry_ref(g_state, &newRef,
                                    "ref entity script module")) {
          mod.lastFailedMtime = currentMtime;
          clear_saved_states_for_module(i);
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
        note_script_reloaded(path);

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
      report_module_refusal(path);
      return LUA_NOREF;
    }
    clear_saved_states_for_module(slot);
  }

  EntityScriptModule &mod = g_entityScriptModules[slot];
  mod = EntityScriptModule{};
  std::memcpy(mod.path, stagedPath, sizeof(mod.path));
  mod.lastFailedMtime = -1;
  rebuild_module_index();
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

/// Calls one behaviour's module function with optional fallback and delta
/// time; a hook that fails faults that behaviour alone.
bool call_module_function(int moduleRef, std::size_t moduleSlot,
                          const char *funcName, const char *fallbackName,
                          runtime::Entity entity, bool hasDt,
                          float dt) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return false;
  }

  // The instruction budget is shared by the frame, so once one script has
  // spent it every later hook would fail at its first instruction. Those
  // scripts did nothing wrong: they are skipped, not faulted, and run again
  // next frame, so which scripts run in an over-budget frame is decided by
  // the dispatch order and the budget alone. Only a hook whose own run
  // fails -- an error, or the run that exhausted the budget -- faults its
  // behaviour.
  if (skip_dispatch_for_spent_budget(funcName)) {
    return false;
  }
  ModuleCallArgs args{};
  args.moduleRef = moduleRef;
  args.moduleSlot = moduleSlot;
  args.funcName = funcName;
  args.fallbackName = fallbackName;
  args.entity = entity;
  args.hasDt = hasDt;
  args.dt = dt;
  if (!protected_hook_dispatch(&module_call_trampoline, &args, 0, funcName)) {
    mark_behaviour_faulted(entity, moduleSlot);
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
ReloadHookResult call_module_reload_hook(int moduleRef, std::size_t moduleSlot,
                                         runtime::Entity entity,
                                         int savedStateRef) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return ReloadHookResult::Failed;
  }

  ModuleCallArgs args{};
  args.moduleRef = moduleRef;
  args.moduleSlot = moduleSlot;
  args.entity = entity;
  args.savedStateRef = savedStateRef;
  if (!protected_hook_dispatch(&module_reload_trampoline, &args, 0,
                               "on_reload")) {
    mark_behaviour_faulted(entity, moduleSlot);
    return ReloadHookResult::Failed;
  }
  return args.called ? ReloadHookResult::Succeeded : ReloadHookResult::Missing;
}

/// Delivers pending module reloads before any new-module tick callback, to
/// every live behaviour (enabled, or begun) that runs a reloaded module.
/// Each
/// module's delivery walk runs over a pre-walk snapshot with per-entity
/// revalidation (alive + the module still listed) so a reload hook that
/// destroys or creates scripted entities mid-walk still delivers to every
/// surviving pre-walk behaviour exactly once; entities created during the
/// walk are excluded by the snapshot. The walk cannot nest with itself
/// (only C callers reach it), so one buffer suffices.
void dispatch_pending_entity_reloads() noexcept {
  if (!g_hasPendingEntityReloads || (g_state == nullptr) || !runtime_bound()) {
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
      BehaviourSnapshot list{};
      if (!world_entity_alive(world, entity) ||
          !snapshot_entity_behaviours(world, entity, &list) ||
          !behaviour_is_live(entity, i, modPath, list)) {
        continue;
      }
      BehaviourState *state = claim_behaviour_state(entity, i, list);
      if (state == nullptr) {
        continue;
      }

      // The new code gets a clean slate: a fault the old code raised no
      // longer stops this behaviour.
      state->flags = static_cast<std::uint8_t>(state->flags &
                                               ~kBehaviourFaulted);
      const int savedStateRef = state->savedStateRef;
      const ReloadHookResult result =
          call_module_reload_hook(module.registryRef, i, entity, savedStateRef);
      if (result == ReloadHookResult::Missing) {
        // Without on_reload the new code starts over from on_begin_play,
        // which also counts as this behaviour beginning.
        if (BehaviourState *restarted = find_behaviour_state(entity, i)) {
          restarted->flags |= kBehaviourBegun;
        }
        static_cast<void>(call_module_function(module.registryRef, i,
                                               "on_begin_play", "on_start",
                                               entity, false, 0.0F));
      }
      if (BehaviourState *after = find_behaviour_state(entity, i)) {
        release_saved_state(*after);
      }
    }

    clear_saved_states_for_module(i);
    module.reloaded = false;
  }
}

/// Loads `path` and reports its module slot; LUA_NOREF when it does not
/// load.
int load_behaviour_module(const char *path, std::size_t *outSlot) noexcept {
  const int ref = get_or_load_entity_script_module(path);
  *outSlot = (ref == LUA_NOREF) ? kInvalidModuleSlot : find_module_slot(path);
  return (*outSlot == kInvalidModuleSlot) ? LUA_NOREF : ref;
}

/// Calls on_end_play on each behaviour of `entity` that began play, in list
/// order: the hooks pair or neither fires. A behaviour disabled after it
/// began still ends.
void end_entity_behaviours(runtime::World *world,
                           runtime::Entity entity) noexcept {
  BehaviourSnapshot list{};
  if (!snapshot_entity_behaviours(world, entity, &list)) {
    return;
  }
  for (std::size_t b = 0U; b < list.count; ++b) {
    if (!world_entity_alive(world, entity)) {
      return;
    }
    const std::size_t slot = find_module_slot(list.paths[b]);
    if ((slot == kInvalidModuleSlot) ||
        !behaviour_has(entity, slot, kBehaviourBegun) ||
        behaviour_has(entity, slot, kBehaviourFaulted)) {
      continue;
    }
    arm_debug_lua_hook(g_state);
    std::size_t loadedSlot = kInvalidModuleSlot;
    const int ref = load_behaviour_module(list.paths[b], &loadedSlot);
    if (ref == LUA_NOREF) {
      continue;
    }
    static_cast<void>(call_module_function(ref, loadedSlot, "on_end_play",
                                           "on_end", entity, false, 0.0F));
  }
}

/// Calls on_begin_play on one behaviour, marking it begun first so a hook
/// that re-enters dispatch does not begin it twice.
void begin_behaviour(int ref, std::size_t moduleSlot, runtime::Entity entity,
                     const BehaviourSnapshot &list) noexcept {
  BehaviourState *state = claim_behaviour_state(entity, moduleSlot, list);
  if (state == nullptr) {
    return;
  }
  state->flags |= kBehaviourBegun;
  call_module_function(ref, moduleSlot, "on_begin_play", "on_start", entity,
                       false, 0.0F);
}

/// Begins play for every enabled behaviour of `entity` that has not begun
/// and has not faulted. Returns true when none is left waiting: each one
/// began, or its module failed to load (false then, so the entity stays
/// pending and its begin-play retries the module next frame).
bool begin_entity_behaviours(runtime::World *world,
                             runtime::Entity entity) noexcept {
  BehaviourSnapshot list{};
  if (!snapshot_entity_behaviours(world, entity, &list)) {
    return true;
  }
  int refs[kMaxBehaviours] = {};
  std::size_t slots[kMaxBehaviours] = {};
  bool allLoaded = true;
  for (std::size_t b = 0U; b < list.count; ++b) {
    refs[b] = LUA_NOREF;
    slots[b] = kInvalidModuleSlot;
    if (!list.enabled[b]) {
      continue;
    }
    const std::size_t known = find_module_slot(list.paths[b]);
    if ((known != kInvalidModuleSlot) &&
        (behaviour_has(entity, known, kBehaviourBegun) ||
         behaviour_has(entity, known, kBehaviourFaulted))) {
      continue;
    }
    arm_debug_lua_hook(g_state);
    refs[b] = load_behaviour_module(list.paths[b], &slots[b]);
    allLoaded = allLoaded && (refs[b] != LUA_NOREF);
  }
  if (allLoaded) {
    runtime_binding().services->mark_begin_play_done(world, entity);
  }
  for (std::size_t b = 0U; b < list.count; ++b) {
    if ((refs[b] == LUA_NOREF) || !world_entity_alive(world, entity)) {
      continue;
    }
    // An earlier behaviour's hook may have hot-reloaded this module, which
    // releases the reference loaded above; a loaded module keeps its slot.
    begin_behaviour(g_entityScriptModules[slots[b]].registryRef, slots[b],
                    entity, list);
  }
  return allLoaded;
}

/// Fires on_end_play for one entity's behaviours that began play.
void dispatch_entity_end_play(runtime::World *world,
                              runtime::Entity entity) noexcept {
  if (!runtime_bound()) {
    return;
  }
  ++g_endPlayDispatchDepth;
  end_entity_behaviours(world, entity);
  --g_endPlayDispatchDepth;
}

/// Bridge visitor: fires on_end_play for one entity of the walked set.
void end_play_visit(core::Entity entity, void *context) noexcept {
  dispatch_entity_end_play(static_cast<runtime::World *>(context), entity);
}

/// Bridge visitor state for one begin-play snapshot pass.
struct BeginPlaySnapshot final {
  runtime::World *world = nullptr;
  std::size_t count = 0U;
};

/// True when some enabled behaviour of `entity` has neither begun nor
/// faulted, so its begin-play has a hook to call. Calls no Lua.
bool has_behaviour_to_begin(runtime::World *world,
                            runtime::Entity entity) noexcept {
  BehaviourSnapshot list{};
  if (!snapshot_entity_behaviours(world, entity, &list)) {
    return false;
  }
  for (std::size_t b = 0U; b < list.count; ++b) {
    if (!list.enabled[b]) {
      continue;
    }
    const std::size_t slot = find_module_slot(list.paths[b]);
    if ((slot == kInvalidModuleSlot) ||
        (!behaviour_has(entity, slot, kBehaviourBegun) &&
         !behaviour_has(entity, slot, kBehaviourFaulted))) {
      return true;
    }
  }
  return false;
}

/// Bridge visitor: settles an entity with nothing to call (no behaviour, or
/// only disabled, begun or faulted ones) on the spot, and records one this
/// dispatch has not attempted yet. Nothing here calls Lua, so the walk sees
/// no creation or destruction.
void begin_play_snapshot_visit(core::Entity entity, void *context) noexcept {
  auto *snapshot = static_cast<BeginPlaySnapshot *>(context);
  if (!has_behaviour_to_begin(snapshot->world, entity)) {
    runtime_binding().services->mark_begin_play_done(snapshot->world, entity);
    return;
  }
  if ((entity.index >= kMaxFaultedEntities) ||
      (g_beginPlayAttempt[entity.index] == g_beginPlayDispatch) ||
      (snapshot->count >= kMaxScriptDispatchEntries)) {
    return;
  }
  g_beginPlayAttempt[entity.index] = g_beginPlayDispatch;
  g_beginPlayOrder[snapshot->count] = entity;
  ++snapshot->count;
}

/// Begins play for one snapshotted entity that still needs it. A module
/// that fails to load leaves the entity pending.
void begin_play_entity(runtime::World *world, core::Entity entity) noexcept {
  if (!world_entity_alive(world, entity) ||
      runtime_binding().services->has_begun_play(world, entity)) {
    return;
  }
  static_cast<void>(begin_entity_behaviours(world, entity));
}

} // namespace

void configure_entity_script_bindings(
    lua_State *state, const EntityScriptBindingCallbacks &callbacks) noexcept {
  g_state = state;
  g_callbacks = callbacks;
}

void clear_entity_script_bindings() noexcept {
  g_state = nullptr;
  g_callbacks = {};
}

bool push_entity_script_module(lua_State *state, const char *path) noexcept {
  if ((state == nullptr) || (path == nullptr) || (path[0] == '\0')) {
    return false;
  }
  const int ref = get_or_load_entity_script_module(path);
  if (ref == LUA_NOREF) {
    return false;
  }
  lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
  return true;
}

const char *running_hook_script(core::Entity entity) noexcept {
  if ((g_hookEntity != entity) || (g_hookModuleSlot >= g_entityScriptModuleCount)) {
    return nullptr;
  }
  return g_entityScriptModules[g_hookModuleSlot].path;
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
  if ((g_state == nullptr) || !runtime_bound()) {
    return;
  }
  ++g_modulePollSerial;

  runtime::World *world = runtime_binding().world;
  const std::size_t count = snapshot_script_dispatch_order();
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = g_scriptDispatchOrder[i];
    if (world_entity_alive(world, entity)) {
      static_cast<void>(begin_entity_behaviours(world, entity));
    }
  }
}

void dispatch_entity_scripts_begin_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr) || !runtime_bound()) {
    return;
  }
  ENGINE_ASSERT_MAIN_THREAD();
  ++g_modulePollSerial;
  ++g_beginPlayDispatch;
  if (g_beginPlayDispatch == 0U) {
    // Zero is every index's initial stamp; skipping it keeps a wrapped
    // counter from reading all of them as already attempted.
    g_beginPlayDispatch = 1U;
  }

  // Each pass snapshots the pending entities first and calls Lua after, so
  // a callback may create or destroy entities: what it spawns is pending
  // for the next pass, and what it destroys is skipped.
  for (std::size_t pass = 0U; pass < kMaxBeginPlayPasses; ++pass) {
    BeginPlaySnapshot snapshot{};
    snapshot.world = world;
    runtime_binding().services->for_each_needs_begin_play(
        world, &begin_play_snapshot_visit, &snapshot);
    if (snapshot.count == 0U) {
      return;
    }
    for (std::size_t i = 0U; i < snapshot.count; ++i) {
      begin_play_entity(world, g_beginPlayOrder[i]);
    }
  }
}

bool in_end_play_dispatch() noexcept { return g_endPlayDispatchDepth > 0; }

void dispatch_entity_subtree_end_play(runtime::World *world,
                                      runtime::Entity entity) noexcept {
  if ((g_state == nullptr) || (world == nullptr) || !runtime_bound()) {
    return;
  }
  runtime_binding().services->for_each_subtree_member(world, entity,
                                                      &end_play_visit, world);
}

void dispatch_entity_scripts_end_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr) || !runtime_bound()) {
    return;
  }
  ++g_modulePollSerial;

  runtime_binding().services->for_each_pending_destroy(world, &end_play_visit,
                                                       world);
}

std::uint64_t entity_script_mtime_polls() noexcept { return g_mtimePolls; }

namespace {

/// Shared body of the per-frame and per-step dispatches: calls `funcName`
/// (or `fallbackName`, when given and the module lacks the first) with
/// the entity and `dt` on every enabled, unfaulted behaviour of every
/// alive scripted entity, entities in snapshotted dispatch order and each
/// entity's behaviours in list order. A behaviour of an entity that has
/// begun play but has not begun itself -- enabled or added since -- begins
/// first, as Unity calls Start before a component's first Update.
void dispatch_entity_scripts_tick(const char *funcName,
                                  const char *fallbackName, float dt) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if ((g_state == nullptr) || !runtime_bound()) {
    return;
  }

  ++g_modulePollSerial;
  dispatch_pending_entity_reloads();

  runtime::World *world = runtime_binding().world;
  const std::size_t count = snapshot_script_dispatch_order();
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = g_scriptDispatchOrder[i];
    BehaviourSnapshot list{};
    if (!world_entity_alive(world, entity) ||
        !snapshot_entity_behaviours(world, entity, &list)) {
      continue;
    }
    for (std::size_t b = 0U; b < list.count; ++b) {
      if (!list.enabled[b]) {
        continue;
      }
      arm_debug_lua_hook(g_state);
      std::size_t slot = kInvalidModuleSlot;
      if (load_behaviour_module(list.paths[b], &slot) == LUA_NOREF) {
        continue;
      }
      dispatch_pending_entity_reloads();
      if (!world_entity_alive(world, entity) ||
          behaviour_has(entity, slot, kBehaviourFaulted)) {
        continue;
      }
      if (!behaviour_has(entity, slot, kBehaviourBegun) &&
          runtime_binding().services->has_begun_play(world, entity)) {
        begin_behaviour(g_entityScriptModules[slot].registryRef, slot, entity,
                        list);
        if (!world_entity_alive(world, entity) ||
            behaviour_has(entity, slot, kBehaviourFaulted)) {
          continue;
        }
      }
      call_module_function(g_entityScriptModules[slot].registryRef, slot,
                           funcName, fallbackName, entity, true, dt);
    }
  }
}

} // namespace

void dispatch_entity_scripts_update(float dt) noexcept {
  dispatch_entity_scripts_tick("on_tick", "on_update", dt);
}

void dispatch_entity_scripts_fixed_update(float dt) noexcept {
  // No legacy name: the hook is new, and a module without it is skipped.
  dispatch_entity_scripts_tick("on_fixed_tick", nullptr, dt);
}

namespace {

/// Shared body of dispatch_entity_scripts_end() and
/// dispatch_entity_scripts_end_for_transition(): calls on_end_play on every
/// behaviour that began play, on every alive scripted entity in the bound
/// world, in snapshotted dispatch order. A behaviour that never began
/// (spawned in the final tick, begin-play still pending, or disabled
/// throughout) gets no on_end_play either, like the destroy path: the
/// hooks pair or neither fires.
void dispatch_entity_scripts_end_impl(runtime::World *world) noexcept {
  const std::size_t count = snapshot_script_dispatch_order();
  for (std::size_t i = 0U; i < count; ++i) {
    const runtime::Entity entity = g_scriptDispatchOrder[i];
    if (world_entity_alive(world, entity)) {
      end_entity_behaviours(world, entity);
    }
  }
}

} // namespace

void dispatch_entity_scripts_end() noexcept {
  if ((g_state == nullptr) || !runtime_bound()) {
    return;
  }
  ++g_modulePollSerial;
  dispatch_entity_scripts_end_impl(runtime_binding().world);
}

void dispatch_entity_scripts_end_for_transition() noexcept {
  if ((g_state == nullptr) || !runtime_bound()) {
    return;
  }
  ++g_modulePollSerial;
  // Both reentrancy holes are closed for the duration of this dispatch:
  // g_endPlayDispatchDepth makes can_apply_mutations_now() defer (not
  // apply) any world mutation a handler triggers (spawn/destroy/etc.), and
  // the teardown-dispatch flag makes engine.load_scene/new_scene reject a
  // handler's own transition request instead of overwriting the one this
  // dispatch is servicing. Mutations deferred here carry the outgoing
  // scene's content epoch; the replacement World restarts entity
  // generations, so the epoch (not the handle's generation) is what makes
  // the next flush drop them instead of retargeting the new scene's
  // entities at the same indices.
  begin_scene_teardown_dispatch();
  ++g_endPlayDispatchDepth;
  dispatch_entity_scripts_end_impl(runtime_binding().world);
  --g_endPlayDispatchDepth;
  end_scene_teardown_dispatch();
}

void clear_entity_script_modules() noexcept {
  clear_lock_rotation_captures();
  clear_behaviour_states();
  g_reportedRefusalCount = 0U;
  g_hasPendingEntityReloads = false;
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
  rebuild_module_index();
}

void reset_entity_script_bindings() noexcept {
  clear_entity_script_modules();
  g_moduleLoadDepth = 0U;
  g_captureDepth = 0U;
}

} // namespace engine::scripting
