#include "engine/scripting/scripting.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "engine/core/console.h"
#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/logging.h"
#include "engine/core/service_locator.h"
#include "engine/core/touch_input.h"
#include "engine/math/quat.h"
#include "engine/runtime/entity_pool.h"
#include "engine/runtime/game_mode.h"
#include "engine/runtime/game_state.h"
#include "engine/runtime/player_controller.h"
#include "engine/runtime/scripting_bridge.h"
#include "engine/runtime/world.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace engine::scripting {

namespace {

lua_State *g_state = nullptr;
runtime::World *g_world = nullptr;
const RuntimeServices *g_services = nullptr;
std::uint32_t g_defaultMeshAssetId = 0U;
std::uint32_t g_builtinPlaneMesh = 0U;
std::uint32_t g_builtinCubeMesh = 0U;
std::uint32_t g_builtinSphereMesh = 0U;
std::uint32_t g_builtinCylinderMesh = 0U;
std::uint32_t g_builtinCapsuleMesh = 0U;
std::uint32_t g_builtinPyramidMesh = 0U;
constexpr math::Vec3 kDefaultGravity(0.0F, -9.8F, 0.0F);
float g_deltaSeconds = 0.0F;
float g_totalSeconds = 0.0F;
std::uint32_t g_frameIndex = 0U;
char g_watchedPath[512] = {};
std::int64_t g_watchedMtime = 0;
constexpr float kMaxScriptAcceleration = 500.0F;

// Entity pool storage for Lua pool_create / pool_spawn / pool_release.
constexpr std::size_t kMaxEntityPools = 16U;
runtime::EntityPool g_entityPools[kMaxEntityPools]{};
std::size_t g_entityPoolCount = 0U;

void copy_c_string(char *destination, std::size_t destinationSize,
                   const char *source) noexcept {
  if ((destination == nullptr) || (destinationSize == 0U)) {
    return;
  }

  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  const std::size_t sourceLength = std::strlen(source);
  const std::size_t copyLength = (sourceLength < (destinationSize - 1U))
                                     ? sourceLength
                                     : (destinationSize - 1U);
  if (copyLength > 0U) {
    std::memcpy(destination, source, copyLength);
  }
  destination[copyLength] = '\0';
}

void copy_clone_name(char *destination, std::size_t destinationSize,
                     const char *source) noexcept {
  constexpr const char *kCloneSuffix = " (clone)";
  constexpr std::size_t kCloneSuffixLength = 8U;

  if ((destination == nullptr) || (destinationSize == 0U)) {
    return;
  }

  destination[0] = '\0';
  if (source == nullptr) {
    return;
  }

  const std::size_t maxPrefixLength =
      (destinationSize > (kCloneSuffixLength + 1U))
          ? (destinationSize - kCloneSuffixLength - 1U)
          : 0U;
  const std::size_t sourceLength = std::strlen(source);
  const std::size_t prefixLength =
      (sourceLength < maxPrefixLength) ? sourceLength : maxPrefixLength;
  if (prefixLength > 0U) {
    std::memcpy(destination, source, prefixLength);
  }
  if ((prefixLength + kCloneSuffixLength) < destinationSize) {
    std::memcpy(destination + prefixLength, kCloneSuffix, kCloneSuffixLength);
    destination[prefixLength + kCloneSuffixLength] = '\0';
  }
}

// --- Entity script module registry ---
struct EntityScriptModule final {
  char path[128] = {};
  int registryRef = LUA_NOREF;
  std::int64_t mtime = 0;
  bool reloaded = false;
};
constexpr std::size_t kMaxEntityScriptModules = 32U;
EntityScriptModule g_entityScriptModules[kMaxEntityScriptModules]{};
std::size_t g_entityScriptModuleCount = 0U;

// Per-entity faulted tracking: if a lifecycle callback errors, skip future
// calls for that entity to avoid cascading log spam.
constexpr std::size_t kMaxFaultedEntities = ENGINE_MAX_ENTITIES + 1U;
bool g_entityFaulted[kMaxFaultedEntities]{};

enum class DeferredMutationType : std::uint8_t {
  DestroyEntity,
  SetTransform,
  AddRigidBody,
  AddCollider,
  AddMeshComponent,
  AddNameComponent,
  AddLightComponent,
  RemoveLightComponent,
  AddScriptComponent,
  RemoveScriptComponent,
};

struct DeferredMutation final {
  DeferredMutationType type = DeferredMutationType::DestroyEntity;
  runtime::Entity entity{};
  runtime::Transform transform{};
  runtime::RigidBody rigidBody{};
  runtime::Collider collider{};
  runtime::MeshComponent meshComponent{};
  runtime::NameComponent nameComponent{};
  runtime::LightComponent lightComponent{};
  runtime::ScriptComponent scriptComponent{};
  runtime::MovementAuthority movementAuthority =
      runtime::MovementAuthority::None;
  bool setMovementAuthority = false;
};

constexpr std::size_t kMaxDeferredMutations = 2048U;
DeferredMutation g_deferredMutations[kMaxDeferredMutations]{};
std::size_t g_deferredMutationCount = 0U;

std::int64_t get_file_mtime(const char *path) noexcept;

constexpr std::size_t kMaxModuleLoadDepth = 32U;
char g_moduleLoadStack[kMaxModuleLoadDepth][128]{};
std::size_t g_moduleLoadDepth = 0U;

constexpr std::size_t kMaxProfilerEntries = 256U;
struct ProfilerEntry final {
  char name[96] = {};
  std::uint32_t samples = 0U;
  bool occupied = false;
};
ProfilerEntry g_profilerEntries[kMaxProfilerEntries]{};
bool g_profilerEnabled = false;

constexpr std::size_t kMaxBreakpoints = 64U;
struct DebugBreakpoint final {
  char file[160] = {};
  int line = 0;
  bool active = false;
};
DebugBreakpoint g_breakpoints[kMaxBreakpoints]{};

constexpr std::size_t kMaxDebugWatches = 32U;
char g_watchExprs[kMaxDebugWatches][96]{};
std::size_t g_watchCount = 0U;
char g_lastWatchOutput[1024] = {};
char g_lastCallstack[2048] = {};
char g_lastBreakpointFile[160] = {};
int g_lastBreakpointLine = 0;
std::uint32_t g_breakpointHitCount = 0U;
bool g_debuggerEnabled = false;

char g_gameMode[64] = "default";
char g_gameState[64] = "startup";
bool g_godModeEnabled = false;
bool g_noclipEnabled = false;
constexpr std::size_t kMaxPlayerControllers = 4U;
std::uint32_t g_playerControllerEntities[kMaxPlayerControllers]{};

// Persistent cross-scene game state (survives World resets).
runtime::GameState g_persistentGameState{};

// Player controllers (survive brief World transitions).
runtime::PlayerControllerArray g_playerControllers{};

void refresh_lua_hook() noexcept;
void scripting_debug_hook(lua_State *state, lua_Debug *ar) noexcept;

bool can_apply_mutations_now() noexcept {
  return (g_world != nullptr) && (g_services != nullptr) &&
         (g_services->get_current_phase(g_world) == runtime::WorldPhase::Input);
}

bool queue_deferred_mutation(const DeferredMutation &mutation) noexcept {
  if (g_deferredMutationCount >= kMaxDeferredMutations) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "deferred mutation queue overflow");
    return false;
  }

  g_deferredMutations[g_deferredMutationCount] = mutation;
  ++g_deferredMutationCount;
  return true;
}

bool apply_or_queue_destroy_entity(runtime::Entity entity) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->destroy_entity_op(g_world, entity.index);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::DestroyEntity;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_transform(runtime::Entity entity,
                              const runtime::Transform &transform,
                              bool setAuthority,
                              runtime::MovementAuthority authority) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    const bool transformUpdated =
        g_services->add_transform_op(g_world, entity.index, transform);
    if (!transformUpdated) {
      return false;
    }
    return !setAuthority || g_services->set_movement_authority_op(
                                g_world, entity.index, authority);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::SetTransform;
  mutation.entity = entity;
  mutation.transform = transform;
  mutation.setMovementAuthority = setAuthority;
  mutation.movementAuthority = authority;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_rigid_body(runtime::Entity entity,
                               const runtime::RigidBody &rigidBody) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->add_rigid_body_op(g_world, entity.index, rigidBody);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddRigidBody;
  mutation.entity = entity;
  mutation.rigidBody = rigidBody;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_collider(runtime::Entity entity,
                             const runtime::Collider &collider) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->add_collider_op(g_world, entity.index, collider);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddCollider;
  mutation.entity = entity;
  mutation.collider = collider;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_mesh_component(
    runtime::Entity entity, const runtime::MeshComponent &component) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->add_mesh_component_op(g_world, entity.index, component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddMeshComponent;
  mutation.entity = entity;
  mutation.meshComponent = component;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_name_component(
    runtime::Entity entity, const runtime::NameComponent &component) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->add_name_component_op(g_world, entity.index, component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddNameComponent;
  mutation.entity = entity;
  mutation.nameComponent = component;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_light_component(
    runtime::Entity entity, const runtime::LightComponent &component) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->add_light_component_op(g_world, entity.index, component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddLightComponent;
  mutation.entity = entity;
  mutation.lightComponent = component;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_remove_light_component(runtime::Entity entity) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->remove_light_component_op(g_world, entity.index);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveLightComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_script_component(
    runtime::Entity entity,
    const runtime::ScriptComponent &component) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->add_script_component_op(g_world, entity.index,
                                               component);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::AddScriptComponent;
  mutation.entity = entity;
  mutation.scriptComponent = component;
  return queue_deferred_mutation(mutation);
}

bool apply_or_queue_remove_script_component(runtime::Entity entity) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr)) {
    return false;
  }

  if (can_apply_mutations_now()) {
    return g_services->remove_script_component_op(g_world, entity.index);
  }

  DeferredMutation mutation{};
  mutation.type = DeferredMutationType::RemoveScriptComponent;
  mutation.entity = entity;
  return queue_deferred_mutation(mutation);
}

bool read_entity_index(lua_State *state, int index,
                       std::uint32_t *outIndex) noexcept {
  if ((outIndex == nullptr) || !lua_isnumber(state, index)) {
    return false;
  }

  const lua_Integer rawIndex = lua_tointeger(state, index);
  if ((rawIndex <= 0) ||
      (rawIndex > static_cast<lua_Integer>(runtime::World::kMaxEntities))) {
    return false;
  }

  *outIndex = static_cast<std::uint32_t>(rawIndex);
  return true;
}

bool read_vec3_args(lua_State *state, int startIndex,
                    math::Vec3 *outVec) noexcept {
  if ((outVec == nullptr) || !lua_isnumber(state, startIndex) ||
      !lua_isnumber(state, startIndex + 1) ||
      !lua_isnumber(state, startIndex + 2)) {
    return false;
  }

  const float x = static_cast<float>(lua_tonumber(state, startIndex));
  const float y = static_cast<float>(lua_tonumber(state, startIndex + 1));
  const float z = static_cast<float>(lua_tonumber(state, startIndex + 2));
  *outVec = math::Vec3(x, y, z);
  return true;
}

bool read_entity(lua_State *state, int index,
                 runtime::Entity *outEntity) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr) ||
      (outEntity == nullptr)) {
    return false;
  }

  std::uint32_t entityIndex = 0U;
  if (!read_entity_index(state, index, &entityIndex)) {
    return false;
  }

  const std::uint32_t validIndex =
      g_services->get_entity_index(g_world, entityIndex);
  if (validIndex == 0U) {
    return false;
  }

  const runtime::Entity resolved = g_world->find_entity_by_index(validIndex);
  if (resolved == runtime::kInvalidEntity) {
    return false;
  }

  *outEntity = resolved;
  return true;
}

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

void profiler_record_sample(const char *name) noexcept {
  if ((name == nullptr) || (name[0] == '\0')) {
    name = "<anonymous>";
  }

  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    if (!g_profilerEntries[i].occupied) {
      continue;
    }
    if (std::strcmp(g_profilerEntries[i].name, name) == 0) {
      ++g_profilerEntries[i].samples;
      return;
    }
  }

  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    if (g_profilerEntries[i].occupied) {
      continue;
    }
    std::snprintf(g_profilerEntries[i].name, sizeof(g_profilerEntries[i].name),
                  "%s", name);
    g_profilerEntries[i].samples = 1U;
    g_profilerEntries[i].occupied = true;
    return;
  }
}

bool debugger_line_matches(const char *source, int line) noexcept {
  if ((source == nullptr) || (line <= 0)) {
    return false;
  }

  const char *normalizedSource = source;
  if (normalizedSource[0] == '@') {
    ++normalizedSource;
  }

  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    if (!g_breakpoints[i].active || (g_breakpoints[i].line != line)) {
      continue;
    }
    const char *bp = g_breakpoints[i].file;
    const std::size_t srcLen = std::strlen(normalizedSource);
    const std::size_t bpLen = std::strlen(bp);
    if ((bpLen > 0U) && (srcLen >= bpLen) &&
        (std::strcmp(normalizedSource + (srcLen - bpLen), bp) == 0)) {
      return true;
    }
  }
  return false;
}

void debugger_capture_watch_values() noexcept {
  g_lastWatchOutput[0] = '\0';
  if ((g_state == nullptr) || (g_watchCount == 0U)) {
    return;
  }

  lua_sethook(g_state, nullptr, 0, 0);

  std::size_t writeOffset = 0U;
  for (std::size_t i = 0U; i < g_watchCount; ++i) {
    const char *expr = g_watchExprs[i];
    char chunk[160] = {};
    std::snprintf(chunk, sizeof(chunk), "return (%s)", expr);

    const int loadStatus = luaL_loadstring(g_state, chunk);
    if (loadStatus != LUA_OK) {
      lua_pop(g_state, 1);
      continue;
    }

    const int callStatus = lua_pcall(g_state, 0, 1, 0);
    const char *value = "<error>";
    char valueBuffer[128] = {};
    if (callStatus == LUA_OK) {
      if (lua_isnumber(g_state, -1)) {
        std::snprintf(valueBuffer, sizeof(valueBuffer), "%g",
                      static_cast<double>(lua_tonumber(g_state, -1)));
        value = valueBuffer;
      } else if (lua_isboolean(g_state, -1)) {
        value = lua_toboolean(g_state, -1) ? "true" : "false";
      } else if (lua_isstring(g_state, -1)) {
        value = lua_tostring(g_state, -1);
      } else if (lua_isnil(g_state, -1)) {
        value = "nil";
      } else {
        value = luaL_typename(g_state, -1);
      }
      lua_pop(g_state, 1);
    } else {
      lua_pop(g_state, 1);
    }

    if (writeOffset < (sizeof(g_lastWatchOutput) - 1U)) {
      const int written =
          std::snprintf(g_lastWatchOutput + writeOffset,
                        sizeof(g_lastWatchOutput) - writeOffset, "%s%s=%s",
                        (writeOffset == 0U) ? "" : "; ", expr, value);
      if (written > 0) {
        const std::size_t delta = static_cast<std::size_t>(written);
        writeOffset = (writeOffset + delta < sizeof(g_lastWatchOutput))
                          ? (writeOffset + delta)
                          : (sizeof(g_lastWatchOutput) - 1U);
      }
    }
  }

  refresh_lua_hook();
}

void scripting_debug_hook(lua_State *state, lua_Debug *ar) noexcept {
  if ((state == nullptr) || (ar == nullptr)) {
    return;
  }

  if (g_profilerEnabled && (ar->event == LUA_HOOKCALL)) {
    if (lua_getinfo(state, "n", ar) != 0) {
      profiler_record_sample((ar->name != nullptr) ? ar->name : "<anonymous>");
    }
  }

  if (g_debuggerEnabled && (ar->event == LUA_HOOKLINE)) {
    if (lua_getinfo(state, "Sln", ar) == 0) {
      return;
    }
    if (!debugger_line_matches(ar->source, ar->currentline)) {
      return;
    }

    const char *source = (ar->source != nullptr) ? ar->source : "";
    if (source[0] == '@') {
      ++source;
    }
    std::snprintf(g_lastBreakpointFile, sizeof(g_lastBreakpointFile), "%s",
                  source);
    g_lastBreakpointLine = ar->currentline;
    ++g_breakpointHitCount;

    luaL_traceback(state, state, "breakpoint", 1);
    const char *trace = lua_tostring(state, -1);
    if (trace != nullptr) {
      std::snprintf(g_lastCallstack, sizeof(g_lastCallstack), "%s", trace);
    }
    lua_pop(state, 1);
    debugger_capture_watch_values();
  }
}

void refresh_lua_hook() noexcept {
  if (g_state == nullptr) {
    return;
  }

  int mask = 0;
  if (g_profilerEnabled) {
    mask |= LUA_MASKCALL;
  }
  if (g_debuggerEnabled) {
    mask |= LUA_MASKLINE;
  }

  if (mask == 0) {
    lua_sethook(g_state, nullptr, 0, 0);
    return;
  }

  lua_sethook(g_state, &scripting_debug_hook, mask, 0);
}

void log_lua_error(const char *context) noexcept {
  if (g_state == nullptr) {
    return;
  }

  const char *message = lua_tostring(g_state, -1);
  if (message == nullptr) {
    message = "unknown lua error";
  }

  // Attach traceback so logs include script file and line diagnostics.
  luaL_traceback(g_state, g_state, message, 1);
  const char *trace = lua_tostring(g_state, -1);
  if (trace == nullptr) {
    trace = message;
  }

  char logBuffer[1024] = {};
  if ((context != nullptr) && (context[0] != '\0')) {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error (%s): %s", context,
                  trace);
  } else {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error: %s", trace);
  }
  core::log_message(core::LogLevel::Error, "scripting", logBuffer);
  lua_pop(g_state, 2);
}

int lua_engine_log(lua_State *state) noexcept {
  const char *message = lua_tostring(state, 1);
  if (message == nullptr) {
    message = "";
  }

  core::log_message(core::LogLevel::Info, "script", message);
  return 0;
}

int lua_engine_get_entity_count(lua_State *state) noexcept {
  const std::size_t count = (g_world != nullptr && g_services != nullptr)
                                ? g_services->get_transform_count(g_world)
                                : 0U;
  lua_pushinteger(state, static_cast<lua_Integer>(count));
  return 1;
}

int lua_engine_spawn_entity(lua_State *state) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr) ||
      !can_apply_mutations_now()) {
    lua_pushnil(state);
    return 1;
  }

  // Only valid during Input; create_entity returns 0 otherwise.
  const std::uint32_t entityIndex = g_services->create_entity_op(g_world);
  if (entityIndex == 0U) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(entityIndex));
  return 1;
}

int lua_engine_destroy_entity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const bool ok = apply_or_queue_destroy_entity(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_is_alive(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }

  std::uint32_t entityIndex = 0U;
  if (!read_entity_index(state, 1, &entityIndex)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const runtime::Entity entity = g_world->find_entity_by_index(entityIndex);
  lua_pushboolean(state, (entity != runtime::kInvalidEntity) ? 1 : 0);
  return 1;
}

int lua_engine_get_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  if (g_services == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Transform *transform =
      g_services->get_transform_read_ptr(g_world, entity.index);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(transform->position.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->position.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->position.z));
  return 3;
}

int lua_engine_set_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 position{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &position)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  if (g_services != nullptr) {
    static_cast<void>(
        g_services->get_transform_op(g_world, entity.index, &transform));
  }
  transform.position = position;

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.z));
  return 3;
}

int lua_engine_add_rigid_body(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  if ((lua_gettop(state) >= 2) && !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (lua_isnumber(state, 2)) {
    rigidBody.inverseMass = static_cast<float>(lua_tonumber(state, 2));
  }

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 velocity{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &velocity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_velocity requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.velocity = velocity;

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 acceleration{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &acceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_acceleration requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  // set_acceleration accepts total world acceleration; convert to the
  // runtime's additive term used by physics integration.
  rigidBody.acceleration =
      math::clamp(math::sub(acceleration, kDefaultGravity),
                  -kMaxScriptAcceleration, kMaxScriptAcceleration);

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_additional_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 additionalAcceleration{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &additionalAcceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_additional_acceleration requires an existing "
                      "RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.acceleration = additionalAcceleration;

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.z));
  return 3;
}

int lua_engine_set_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 angVel{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &angVel)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_angular_velocity requires an existing RigidBody");
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.angularVelocity = angVel;

  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer meshId = lua_tointeger(state, 2);
  if ((meshId <= 0) ||
      (meshId >
       static_cast<lua_Integer>(std::numeric_limits<std::uint32_t>::max()))) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  const runtime::MeshComponent *existing =
      g_world->get_mesh_component_ptr(entity);
  if (existing != nullptr) {
    component = *existing;
  }

  component.meshAssetId = static_cast<std::uint32_t>(meshId);
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_default_mesh_asset_id(lua_State *state) noexcept {
  if (g_defaultMeshAssetId == 0U) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(g_defaultMeshAssetId));
  return 1;
}

// engine.spawn_shape(shape, x, y, z, r, g, b) → entity index or nil
// shape: "cube" | "sphere" | "cylinder" | "capsule" | "pyramid" | "plane"
// Spawns a physics entity with the matching mesh and appropriate collider.
int lua_engine_spawn_shape(lua_State *state) noexcept {
  if ((g_world == nullptr) || !can_apply_mutations_now() ||
      !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  math::Vec3 pos{};
  math::Vec3 albedo(1.0F, 1.0F, 1.0F);
  if (!read_vec3_args(state, 2, &pos)) {
    lua_pushnil(state);
    return 1;
  }
  if (lua_isnumber(state, 5) && lua_isnumber(state, 6) &&
      lua_isnumber(state, 7)) {
    albedo.x = static_cast<float>(lua_tonumber(state, 5));
    albedo.y = static_cast<float>(lua_tonumber(state, 6));
    albedo.z = static_cast<float>(lua_tonumber(state, 7));
  }

  const char *shape = lua_tostring(state, 1);

  std::uint32_t meshId = g_defaultMeshAssetId;
  math::Vec3 halfExtents(0.5F, 0.5F, 0.5F);
  runtime::ColliderShape colliderShape = runtime::ColliderShape::AABB;

  if (std::strcmp(shape, "cube") == 0) {
    meshId =
        (g_builtinCubeMesh != 0U) ? g_builtinCubeMesh : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "sphere") == 0) {
    meshId = (g_builtinSphereMesh != 0U) ? g_builtinSphereMesh
                                         : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F); // halfExtents.x = radius
    colliderShape = runtime::ColliderShape::Sphere;
  } else if (std::strcmp(shape, "cylinder") == 0) {
    meshId = (g_builtinCylinderMesh != 0U) ? g_builtinCylinderMesh
                                           : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "capsule") == 0) {
    meshId = (g_builtinCapsuleMesh != 0U) ? g_builtinCapsuleMesh
                                          : g_defaultMeshAssetId;
    // Physics currently supports AABB/Sphere only; capsule uses a conservative
    // AABB fit so the debug volume matches the rendered mesh bounds.
    halfExtents = math::Vec3(0.5F, 1.0F, 0.5F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "pyramid") == 0) {
    meshId = (g_builtinPyramidMesh != 0U) ? g_builtinPyramidMesh
                                          : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.58F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "plane") == 0) {
    meshId =
        (g_builtinPlaneMesh != 0U) ? g_builtinPlaneMesh : g_defaultMeshAssetId;
    halfExtents = math::Vec3(5.0F, 0.1F, 5.0F);
    colliderShape = runtime::ColliderShape::AABB;
  }

  const runtime::Entity entity = g_world->create_entity();
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  runtime::Transform transform{};
  transform.position = pos;
  static_cast<void>(g_world->add_transform(entity, transform));

  runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 1.0F;
  static_cast<void>(g_world->add_rigid_body(entity, rigidBody));

  runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  collider.shape = colliderShape;
  static_cast<void>(g_world->add_collider(entity, collider));

  if (meshId != 0U) {
    runtime::MeshComponent meshComp{};
    meshComp.meshAssetId = meshId;
    meshComp.albedo = albedo;
    static_cast<void>(g_world->add_mesh_component(entity, meshComp));
  }

  lua_pushinteger(state, static_cast<lua_Integer>(entity.index));
  return 1;
}

int lua_engine_set_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 albedo{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &albedo)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  const runtime::MeshComponent *existing =
      g_world->get_mesh_component_ptr(entity);
  if (existing != nullptr) {
    component = *existing;
  }

  component.albedo = albedo;
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_name(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isstring(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 2);
  if (name == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::NameComponent component{};
  const std::size_t nameLength = std::strlen(name);
  constexpr std::size_t kMaxNameLength = sizeof(component.name) - 1U;
  if (nameLength > kMaxNameLength) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_name truncated input to NameComponent capacity");
  }
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  component.name[sizeof(component.name) - 1U] = '\0';

  const bool ok = apply_or_queue_name_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_name(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::NameComponent component{};
  if ((g_services == nullptr) ||
      !g_services->get_name_component_op(g_world, entity.index, &component)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushstring(state, component.name);
  return 1;
}

int lua_engine_add_collider(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 halfExtents{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &halfExtents)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Collider collider{};
  collider.halfExtents = halfExtents;

  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_restitution(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float value = static_cast<float>(lua_tonumber(state, 2));
  runtime::Collider collider{};
  if (!g_world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  collider.restitution = value;
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_set_friction(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2) || !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float staticF = static_cast<float>(lua_tonumber(state, 2));
  const float dynamicF = static_cast<float>(lua_tonumber(state, 3));
  runtime::Collider collider{};
  if (!g_world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  collider.staticFriction = staticF;
  collider.dynamicFriction = dynamicF;
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_delta_time(lua_State *state) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(g_deltaSeconds));
  return 1;
}

int lua_engine_elapsed_time(lua_State *state) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(g_totalSeconds));
  return 1;
}

int lua_engine_is_key_down(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int scancode = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_key_down(scancode) ? 1 : 0);
  return 1;
}

int lua_engine_is_key_pressed(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int scancode = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_key_pressed(scancode) ? 1 : 0);
  return 1;
}

int lua_engine_register_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 1);
  const int key = static_cast<int>(lua_tointeger(state, 2));
  const int mouseButton =
      lua_isnumber(state, 3) ? static_cast<int>(lua_tointeger(state, 3)) : -1;
  const bool ok = core::register_action(name, key, mouseButton);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_register_axis(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *name = lua_tostring(state, 1);
  const int negativeKey = static_cast<int>(lua_tointeger(state, 2));
  const int positiveKey = static_cast<int>(lua_tointeger(state, 3));
  const bool ok = core::register_axis(name, negativeKey, positiveKey);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_is_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_down(name) ? 1 : 0);
  return 1;
}

int lua_engine_is_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_pressed(name) ? 1 : 0);
  return 1;
}

int lua_engine_get_action_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::action_value(name)));
  return 1;
}

int lua_engine_get_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::axis_value(name)));
  return 1;
}

int lua_engine_is_gamepad_connected(lua_State *state) noexcept {
  static_cast<void>(state);
  lua_pushboolean(state, core::is_gamepad_connected() ? 1 : 0);
  return 1;
}

int lua_engine_is_gamepad_button_down(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int button = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_gamepad_button_down(button) ? 1 : 0);
  return 1;
}

int lua_engine_gamepad_axis_value(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushnumber(state, 0.0);
    return 1;
  }
  const int axis = static_cast<int>(lua_tointeger(state, 1));
  const int deadzone =
      lua_isnumber(state, 2) ? static_cast<int>(lua_tointeger(state, 2)) : 8000;
  lua_pushnumber(
      state, static_cast<lua_Number>(core::gamepad_axis_value(axis, deadzone)));
  return 1;
}

// ---------------------------------------------------------------------------
// InputMapper bindings (P1-M2-C)
// ---------------------------------------------------------------------------

// engine.add_input_action(name, {bindings...})
// Each binding: {type=0..3, code=N [, axisThreshold=0.5, axisScale=1]}
int lua_engine_add_input_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_istable(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  core::InputBinding bindings[core::kMaxBindingsPerAction]{};
  std::uint32_t count = 0U;
  const int tableLen = static_cast<int>(lua_rawlen(state, 2));
  for (int i = 1; i <= tableLen && count < core::kMaxBindingsPerAction; ++i) {
    lua_rawgeti(state, 2, i);
    if (lua_istable(state, -1)) {
      lua_getfield(state, -1, "type");
      bindings[count].type =
          static_cast<core::InputBindingType>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "code");
      bindings[count].code = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "axis_threshold");
      if (lua_isnumber(state, -1)) {
        bindings[count].axisThreshold =
            static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      lua_getfield(state, -1, "axis_scale");
      if (lua_isnumber(state, -1)) {
        bindings[count].axisScale = static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      ++count;
    }
    lua_pop(state, 1);
  }
  const bool ok = core::add_input_action(name, bindings, count);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.add_input_axis(name, {sources...})
int lua_engine_add_input_axis(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_istable(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  core::InputAxisSource sources[core::kMaxSourcesPerAxis]{};
  std::uint32_t count = 0U;
  const int tableLen = static_cast<int>(lua_rawlen(state, 2));
  for (int i = 1; i <= tableLen && count < core::kMaxSourcesPerAxis; ++i) {
    lua_rawgeti(state, 2, i);
    if (lua_istable(state, -1)) {
      lua_getfield(state, -1, "type");
      sources[count].type =
          static_cast<core::AxisSourceType>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "negative_key");
      sources[count].negativeKey = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "positive_key");
      sources[count].positiveKey = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "axis_index");
      sources[count].axisIndex = static_cast<int>(lua_tointeger(state, -1));
      lua_pop(state, 1);
      lua_getfield(state, -1, "scale");
      if (lua_isnumber(state, -1)) {
        sources[count].scale = static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      lua_getfield(state, -1, "dead_zone");
      if (lua_isnumber(state, -1)) {
        sources[count].deadZone = static_cast<float>(lua_tonumber(state, -1));
      }
      lua_pop(state, 1);
      ++count;
    }
    lua_pop(state, 1);
  }
  const bool ok = core::add_input_axis(name, sources, count);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.is_mapped_action_down(name)
int lua_engine_is_mapped_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_mapped_action_down(name) ? 1 : 0);
  return 1;
}

// engine.is_mapped_action_pressed(name)
int lua_engine_is_mapped_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_mapped_action_pressed(name) ? 1 : 0);
  return 1;
}

// engine.mapped_axis_value(name)
int lua_engine_mapped_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::mapped_axis_value(name)));
  return 1;
}

// engine.rebind_action(actionName, bindingIndex, {type=N, code=N})
int lua_engine_rebind_action(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isnumber(state, 2) ||
      !lua_istable(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *name = lua_tostring(state, 1);
  const auto bindingIdx = static_cast<std::uint32_t>(lua_tointeger(state, 2));
  core::InputBinding binding{};
  lua_getfield(state, 3, "type");
  binding.type = static_cast<core::InputBindingType>(lua_tointeger(state, -1));
  lua_pop(state, 1);
  lua_getfield(state, 3, "code");
  binding.code = static_cast<int>(lua_tointeger(state, -1));
  lua_pop(state, 1);
  lua_getfield(state, 3, "axis_threshold");
  if (lua_isnumber(state, -1)) {
    binding.axisThreshold = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);
  lua_getfield(state, 3, "axis_scale");
  if (lua_isnumber(state, -1)) {
    binding.axisScale = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);
  const bool ok = core::rebind_action(name, bindingIdx, binding);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.save_input_config(path)
int lua_engine_save_input_config(lua_State *state) noexcept {
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::save_input_bindings(path) ? 1 : 0);
  return 1;
}

// engine.load_input_config(path)
int lua_engine_load_input_config(lua_State *state) noexcept {
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, core::load_input_bindings(path) ? 1 : 0);
  return 1;
}

// ---------------------------------------------------------------------------
// Touch input bindings (P1-M2-C3e)
// ---------------------------------------------------------------------------

// Lua callback registry refs for touch/gesture.
static lua_State *g_touchLuaState = nullptr;
static int g_touchCallbackRef = LUA_NOREF;

static void lua_touch_handler(const core::TouchEvent &event,
                              void * /*userData*/) noexcept {
  if ((g_touchLuaState == nullptr) || (g_touchCallbackRef == LUA_NOREF)) {
    return;
  }
  lua_rawgeti(g_touchLuaState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  if (!lua_isfunction(g_touchLuaState, -1)) {
    lua_pop(g_touchLuaState, 1);
    return;
  }
  lua_newtable(g_touchLuaState);
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.touchId));
  lua_setfield(g_touchLuaState, -2, "id");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.x));
  lua_setfield(g_touchLuaState, -2, "x");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.y));
  lua_setfield(g_touchLuaState, -2, "y");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.pressure));
  lua_setfield(g_touchLuaState, -2, "pressure");
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.phase));
  lua_setfield(g_touchLuaState, -2, "phase");
  if (lua_pcall(g_touchLuaState, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(g_touchLuaState, -1);
    core::log_message(core::LogLevel::Error, "Scripting",
                      err ? err : "touch callback error");
    lua_pop(g_touchLuaState, 1);
  }
}

// engine.on_touch(callback)
int lua_engine_on_touch(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  // Release old ref if any.
  if ((g_touchLuaState != nullptr) && (g_touchCallbackRef != LUA_NOREF)) {
    luaL_unref(g_touchLuaState, LUA_REGISTRYINDEX, g_touchCallbackRef);
  }
  g_touchLuaState = state;
  lua_pushvalue(state, 1);
  g_touchCallbackRef = luaL_ref(state, LUA_REGISTRYINDEX);
  core::register_touch_callback(&lua_touch_handler, nullptr);
  lua_pushboolean(state, 1);
  return 1;
}

// Per-gesture-type Lua callback refs.
static int g_gestureCallbackRefs[4] = {LUA_NOREF, LUA_NOREF, LUA_NOREF,
                                       LUA_NOREF};

static void lua_gesture_handler(const core::GestureEvent &event,
                                void * /*userData*/) noexcept {
  if (g_touchLuaState == nullptr) {
    return;
  }
  const int idx = static_cast<int>(event.type);
  if ((idx < 0) || (idx >= 4) || (g_gestureCallbackRefs[idx] == LUA_NOREF)) {
    return;
  }
  lua_rawgeti(g_touchLuaState, LUA_REGISTRYINDEX, g_gestureCallbackRefs[idx]);
  if (!lua_isfunction(g_touchLuaState, -1)) {
    lua_pop(g_touchLuaState, 1);
    return;
  }
  lua_newtable(g_touchLuaState);
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.type));
  lua_setfield(g_touchLuaState, -2, "type");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.tapX));
  lua_setfield(g_touchLuaState, -2, "tap_x");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.tapY));
  lua_setfield(g_touchLuaState, -2, "tap_y");
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.tapCount));
  lua_setfield(g_touchLuaState, -2, "tap_count");
  lua_pushinteger(g_touchLuaState, static_cast<lua_Integer>(event.swipeDir));
  lua_setfield(g_touchLuaState, -2, "swipe_dir");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.swipeVelocity));
  lua_setfield(g_touchLuaState, -2, "swipe_velocity");
  lua_pushnumber(g_touchLuaState, static_cast<lua_Number>(event.pinchScale));
  lua_setfield(g_touchLuaState, -2, "pinch_scale");
  lua_pushnumber(g_touchLuaState,
                 static_cast<lua_Number>(event.rotationRadians));
  lua_setfield(g_touchLuaState, -2, "rotation");
  if (lua_pcall(g_touchLuaState, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(g_touchLuaState, -1);
    core::log_message(core::LogLevel::Error, "Scripting",
                      err ? err : "gesture callback error");
    lua_pop(g_touchLuaState, 1);
  }
}

// engine.on_gesture(type_string, callback)
// type_string: "tap", "swipe", "pinch", "rotate"
int lua_engine_on_gesture(lua_State *state) noexcept {
  if (!lua_isstring(state, 1) || !lua_isfunction(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *typeStr = lua_tostring(state, 1);
  int idx = -1;
  if (std::strcmp(typeStr, "tap") == 0) {
    idx = 0;
  } else if (std::strcmp(typeStr, "swipe") == 0) {
    idx = 1;
  } else if (std::strcmp(typeStr, "pinch") == 0) {
    idx = 2;
  } else if (std::strcmp(typeStr, "rotate") == 0) {
    idx = 3;
  }
  if (idx < 0) {
    lua_pushboolean(state, 0);
    return 1;
  }

  // Release old ref.
  if ((g_touchLuaState != nullptr) &&
      (g_gestureCallbackRefs[idx] != LUA_NOREF)) {
    luaL_unref(g_touchLuaState, LUA_REGISTRYINDEX, g_gestureCallbackRefs[idx]);
  }
  g_touchLuaState = state;
  lua_pushvalue(state, 2);
  g_gestureCallbackRefs[idx] = luaL_ref(state, LUA_REGISTRYINDEX);
  core::register_gesture_callback(static_cast<core::GestureType>(idx),
                                  &lua_gesture_handler, nullptr);
  lua_pushboolean(state, 1);
  return 1;
}

// engine.set_touch_mouse_emulation(enabled)
int lua_engine_set_touch_mouse_emulation(lua_State *state) noexcept {
  const bool enabled = lua_toboolean(state, 1) != 0;
  core::set_touch_mouse_emulation(enabled);
  return 0;
}

int lua_engine_set_game_mode(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  if (name == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  // Write to both legacy string and World's GameMode struct.
  std::snprintf(g_gameMode, sizeof(g_gameMode), "%s", name);
  if (g_world != nullptr) {
    std::snprintf(g_world->game_mode().name, runtime::GameMode::kMaxNameLength,
                  "%s", name);
  }
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_get_game_mode(lua_State *state) noexcept {
  if (g_world != nullptr) {
    lua_pushstring(state, g_world->game_mode().name);
  } else {
    lua_pushstring(state, g_gameMode);
  }
  return 1;
}

int lua_engine_set_game_state(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  if (name == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  std::snprintf(g_gameState, sizeof(g_gameState), "%s", name);
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_get_game_state(lua_State *state) noexcept {
  lua_pushstring(state, g_gameState);
  return 1;
}

int lua_engine_set_player_controller(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer player = lua_tointeger(state, 1);
  const lua_Integer entityIndex = lua_tointeger(state, 2);
  if ((player < 0) ||
      (player >= static_cast<lua_Integer>(kMaxPlayerControllers)) ||
      (entityIndex < 0) ||
      (entityIndex > static_cast<lua_Integer>(runtime::World::kMaxEntities))) {
    lua_pushboolean(state, 0);
    return 1;
  }

  g_playerControllerEntities[static_cast<std::size_t>(player)] =
      static_cast<std::uint32_t>(entityIndex);
  g_playerControllers.set_controlled_entity(
      static_cast<std::uint8_t>(player),
      static_cast<std::uint32_t>(entityIndex));
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_is_god_mode(lua_State *state) noexcept {
  lua_pushboolean(state, g_godModeEnabled ? 1 : 0);
  return 1;
}

int lua_engine_is_noclip(lua_State *state) noexcept {
  lua_pushboolean(state, g_noclipEnabled ? 1 : 0);
  return 1;
}

int lua_engine_get_player_controller(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer player = lua_tointeger(state, 1);
  if ((player < 0) ||
      (player >= static_cast<lua_Integer>(kMaxPlayerControllers))) {
    lua_pushnil(state);
    return 1;
  }

  const auto idx = static_cast<std::uint8_t>(player);
  lua_pushinteger(state, static_cast<lua_Integer>(
                             g_playerControllers.get_controlled_entity(idx)));
  return 1;
}

// --- Game Mode state transitions ---

int lua_engine_game_mode_start(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_world->game_mode().start() ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_pause(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_world->game_mode().pause() ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_end(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_world->game_mode().end() ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_state(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushstring(state, "none");
    return 1;
  }
  using S = runtime::GameMode::State;
  switch (g_world->game_mode().state) {
  case S::WaitingToStart:
    lua_pushstring(state, "waiting_to_start");
    break;
  case S::InProgress:
    lua_pushstring(state, "in_progress");
    break;
  case S::Paused:
    lua_pushstring(state, "paused");
    break;
  case S::Ended:
    lua_pushstring(state, "ended");
    break;
  }
  return 1;
}

int lua_engine_game_mode_set_rule(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *key = lua_tostring(state, 1);
  const char *value = lua_tostring(state, 2);
  if (key == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_world->game_mode().set_rule(key, value) ? 1 : 0);
  return 1;
}

int lua_engine_game_mode_get_rule(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  const char *key = lua_tostring(state, 1);
  const char *value = g_world->game_mode().get_rule(key);
  if (value != nullptr) {
    lua_pushstring(state, value);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_engine_game_mode_max_players(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushinteger(state, 0);
    return 1;
  }
  if (lua_gettop(state) >= 1 && lua_isnumber(state, 1)) {
    const auto n = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    g_world->game_mode().maxPlayers = n;
  }
  lua_pushinteger(state,
                  static_cast<lua_Integer>(g_world->game_mode().maxPlayers));
  return 1;
}

// --- Persistent GameState ---

int lua_engine_game_state_set_number(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  if ((key == nullptr) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_persistentGameState.set_number(
                             key, static_cast<float>(lua_tonumber(state, 2)))
                             ? 1
                             : 0);
  return 1;
}

int lua_engine_game_state_get_number(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  lua_pushnumber(
      state, static_cast<lua_Number>(g_persistentGameState.get_number(key)));
  return 1;
}

int lua_engine_game_state_set_string(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  const char *value = lua_tostring(state, 2);
  if (key == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_persistentGameState.set_string(key, value) ? 1 : 0);
  return 1;
}

int lua_engine_game_state_get_string(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  const char *value = g_persistentGameState.get_string(key);
  if (value != nullptr) {
    lua_pushstring(state, value);
  } else {
    lua_pushnil(state);
  }
  return 1;
}

int lua_engine_game_state_has(lua_State *state) noexcept {
  const char *key = lua_tostring(state, 1);
  lua_pushboolean(state, g_persistentGameState.has(key) ? 1 : 0);
  return 1;
}

int lua_engine_game_state_clear(lua_State *state) noexcept {
  static_cast<void>(state);
  g_persistentGameState.clear();
  return 0;
}

int lua_engine_profiler_enable(lua_State *state) noexcept {
  g_profilerEnabled =
      (lua_gettop(state) >= 1) && (lua_toboolean(state, 1) != 0);
  refresh_lua_hook();
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_profiler_reset(lua_State *state) noexcept {
  static_cast<void>(state);
  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    g_profilerEntries[i] = ProfilerEntry{};
  }
  return 0;
}

int lua_engine_profiler_get_count(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  if (name == nullptr) {
    lua_pushinteger(state, 0);
    return 1;
  }

  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    if (!g_profilerEntries[i].occupied) {
      continue;
    }
    if (std::strcmp(g_profilerEntries[i].name, name) == 0) {
      lua_pushinteger(state,
                      static_cast<lua_Integer>(g_profilerEntries[i].samples));
      return 1;
    }
  }

  lua_pushinteger(state, 0);
  return 1;
}

int lua_engine_debugger_enable(lua_State *state) noexcept {
  g_debuggerEnabled =
      (lua_gettop(state) >= 1) && (lua_toboolean(state, 1) != 0);
  refresh_lua_hook();
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_debugger_add_breakpoint(lua_State *state) noexcept {
  const char *file = lua_tostring(state, 1);
  if ((file == nullptr) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const int line = static_cast<int>(lua_tointeger(state, 2));
  if (line <= 0) {
    lua_pushboolean(state, 0);
    return 1;
  }

  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    if (!g_breakpoints[i].active) {
      std::snprintf(g_breakpoints[i].file, sizeof(g_breakpoints[i].file), "%s",
                    file);
      g_breakpoints[i].line = line;
      g_breakpoints[i].active = true;
      lua_pushboolean(state, 1);
      return 1;
    }
  }

  lua_pushboolean(state, 0);
  return 1;
}

int lua_engine_debugger_clear_breakpoints(lua_State *state) noexcept {
  static_cast<void>(state);
  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    g_breakpoints[i] = DebugBreakpoint{};
  }
  return 0;
}

int lua_engine_debugger_add_watch(lua_State *state) noexcept {
  const char *expr = lua_tostring(state, 1);
  if ((expr == nullptr) || (g_watchCount >= kMaxDebugWatches)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  std::snprintf(g_watchExprs[g_watchCount], sizeof(g_watchExprs[0]), "%s",
                expr);
  ++g_watchCount;
  lua_pushboolean(state, 1);
  return 1;
}

int lua_engine_debugger_clear_watches(lua_State *state) noexcept {
  static_cast<void>(state);
  g_watchCount = 0U;
  g_lastWatchOutput[0] = '\0';
  return 0;
}

int lua_engine_debugger_last_breakpoint(lua_State *state) noexcept {
  if (g_lastBreakpointLine <= 0) {
    lua_pushnil(state);
    return 1;
  }
  lua_newtable(state);
  lua_pushstring(state, g_lastBreakpointFile);
  lua_setfield(state, -2, "file");
  lua_pushinteger(state, static_cast<lua_Integer>(g_lastBreakpointLine));
  lua_setfield(state, -2, "line");
  lua_pushinteger(state, static_cast<lua_Integer>(g_breakpointHitCount));
  lua_setfield(state, -2, "hits");
  return 1;
}

int lua_engine_debugger_last_callstack(lua_State *state) noexcept {
  lua_pushstring(state, g_lastCallstack);
  return 1;
}

int lua_engine_debugger_last_watch_values(lua_State *state) noexcept {
  lua_pushstring(state, g_lastWatchOutput);
  return 1;
}

int lua_engine_set_camera_position(lua_State *state) noexcept {
  math::Vec3 pos{};
  if (!read_vec3_args(state, 1, &pos) || (g_services == nullptr) ||
      (g_services->set_camera_position == nullptr)) {
    return 0;
  }
  g_services->set_camera_position(pos.x, pos.y, pos.z);
  return 0;
}

int lua_engine_set_camera_target(lua_State *state) noexcept {
  math::Vec3 target{};
  if (!read_vec3_args(state, 1, &target) || (g_services == nullptr) ||
      (g_services->set_camera_target == nullptr)) {
    return 0;
  }
  g_services->set_camera_target(target.x, target.y, target.z);
  return 0;
}

int lua_engine_set_camera_up(lua_State *state) noexcept {
  math::Vec3 up{};
  if (!read_vec3_args(state, 1, &up) || (g_services == nullptr) ||
      (g_services->set_camera_up == nullptr)) {
    return 0;
  }
  g_services->set_camera_up(up.x, up.y, up.z);
  return 0;
}

int lua_engine_set_camera_fov(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || (g_services == nullptr) ||
      (g_services->set_camera_fov == nullptr)) {
    return 0;
  }
  g_services->set_camera_fov(static_cast<float>(lua_tonumber(state, 1)));
  return 0;
}

int lua_engine_set_gravity(lua_State *state) noexcept {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  if (lua_isnumber(state, 1)) {
    x = static_cast<float>(lua_tonumber(state, 1));
  }
  if (lua_isnumber(state, 2)) {
    y = static_cast<float>(lua_tonumber(state, 2));
  }
  if (lua_isnumber(state, 3)) {
    z = static_cast<float>(lua_tonumber(state, 3));
  }
  if ((g_services != nullptr) && (g_services->set_gravity != nullptr)) {
    g_services->set_gravity(g_world, x, y, z);
  }
  return 0;
}

int lua_engine_get_gravity(lua_State *state) noexcept {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  if ((g_services == nullptr) || (g_services->get_gravity == nullptr) ||
      !g_services->get_gravity(g_world, &x, &y, &z)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(x));
  lua_pushnumber(state, static_cast<lua_Number>(y));
  lua_pushnumber(state, static_cast<lua_Number>(z));
  return 3;
}

int lua_engine_raycast(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7)) {
    lua_pushnil(state);
    return 1;
  }
  const float ox = static_cast<float>(lua_tonumber(state, 1));
  const float oy = static_cast<float>(lua_tonumber(state, 2));
  const float oz = static_cast<float>(lua_tonumber(state, 3));
  const float dx = static_cast<float>(lua_tonumber(state, 4));
  const float dy = static_cast<float>(lua_tonumber(state, 5));
  const float dz = static_cast<float>(lua_tonumber(state, 6));
  const float maxDist = static_cast<float>(lua_tonumber(state, 7));

  RuntimeRaycastHit hit{};
  if ((g_services == nullptr) || (g_services->raycast == nullptr) ||
      !g_services->raycast(g_world, ox, oy, oz, dx, dy, dz, maxDist, &hit)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(hit.entityIndex));
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

int lua_engine_add_distance_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (g_services == nullptr) || (g_services->add_distance_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const float dist = lua_isnumber(state, 3)
                         ? static_cast<float>(lua_tonumber(state, 3))
                         : 1.0F;
  const std::uint32_t id = g_services->add_distance_joint(
      g_world, entityA.index, entityB.index, dist);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

int lua_engine_remove_joint(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((g_services != nullptr) && (g_services->remove_joint != nullptr)) {
    g_services->remove_joint(
        g_world, static_cast<std::uint32_t>(lua_tointeger(state, 1)));
  }
  return 0;
}

int lua_engine_wake_body(lua_State *state) noexcept {
  if (g_world == nullptr) {
    return 0;
  }
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((g_services != nullptr) && (g_services->wake_body != nullptr)) {
    const auto idx = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    g_services->wake_body(g_world, idx);
  }
  return 0;
}

int lua_engine_is_sleeping(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if ((g_services == nullptr) || (g_services->is_sleeping == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto idx = static_cast<std::uint32_t>(lua_tointeger(state, 1));
  lua_pushboolean(state, g_services->is_sleeping(g_world, idx) ? 1 : 0);
  return 1;
}

int lua_engine_frame_count(lua_State *state) noexcept {
  lua_pushinteger(state, static_cast<lua_Integer>(g_frameIndex));
  return 1;
}

int lua_engine_load_sound(lua_State *state) noexcept {
  if (!lua_isstring(state, 1)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushinteger(state, 0);
    return 1;
  }
  if ((g_services == nullptr) || (g_services->load_sound == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  lua_pushinteger(state,
                  static_cast<lua_Integer>(g_services->load_sound(path)));
  return 1;
}

int lua_engine_unload_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((g_services != nullptr) && (g_services->unload_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    g_services->unload_sound(id);
  }
  return 0;
}

int lua_engine_play_sound(lua_State *state) noexcept {
  if ((g_services == nullptr) || (g_services->play_sound == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
  float volume = 1.0F;
  float pitch = 1.0F;
  bool loop = false;
  if ((lua_gettop(state) >= 2) && lua_isnumber(state, 2)) {
    volume = static_cast<float>(lua_tonumber(state, 2));
  }
  if ((lua_gettop(state) >= 3) && lua_isnumber(state, 3)) {
    pitch = static_cast<float>(lua_tonumber(state, 3));
  }
  if (lua_gettop(state) >= 4) {
    loop = lua_toboolean(state, 4) != 0;
  }
  const bool ok = g_services->play_sound(id, volume, pitch, loop);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_stop_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((g_services != nullptr) && (g_services->stop_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    g_services->stop_sound(id);
  }
  return 0;
}

int lua_engine_stop_all_sounds(lua_State *state) noexcept {
  static_cast<void>(state);
  if ((g_services != nullptr) && (g_services->stop_all_sounds != nullptr)) {
    g_services->stop_all_sounds();
  }
  return 0;
}

int lua_engine_set_master_volume(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((g_services != nullptr) && (g_services->set_master_volume != nullptr)) {
    const auto vol = static_cast<float>(lua_tonumber(state, 1));
    g_services->set_master_volume(vol);
  }
  return 0;
}

// --- Transform: rotation and scale ---

int lua_engine_get_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::Transform *transform = g_world->get_transform_read_ptr(entity);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.z));
  lua_pushnumber(state, static_cast<lua_Number>(transform->rotation.w));
  return 4;
}

int lua_engine_set_rotation(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2) || !lua_isnumber(state, 3) ||
      !lua_isnumber(state, 4) || !lua_isnumber(state, 5)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float qx = static_cast<float>(lua_tonumber(state, 2));
  const float qy = static_cast<float>(lua_tonumber(state, 3));
  const float qz = static_cast<float>(lua_tonumber(state, 4));
  const float qw = static_cast<float>(lua_tonumber(state, 5));

  runtime::Transform transform{};
  static_cast<void>(g_world->get_transform(entity, &transform));
  transform.rotation = math::Quat(qx, qy, qz, qw);

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::Transform *transform = g_world->get_transform_read_ptr(entity);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.z));
  return 3;
}

int lua_engine_set_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 scale{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &scale)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  static_cast<void>(g_world->get_transform(entity, &transform));
  transform.scale = scale;

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- RigidBody: inverse mass ---

int lua_engine_get_inverse_mass(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.inverseMass));
  return 1;
}

int lua_engine_set_inverse_mass(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  rigidBody.inverseMass = static_cast<float>(lua_tonumber(state, 2));
  const bool ok = apply_or_queue_rigid_body(entity, rigidBody);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- Collider: getters ---

int lua_engine_get_half_extents(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!g_world->get_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.x));
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.y));
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.z));
  return 3;
}

int lua_engine_set_half_extents(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 halfExtents{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &halfExtents)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!g_world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.halfExtents = halfExtents;
  lua_pushboolean(state, apply_or_queue_collider(entity, collider) ? 1 : 0);
  return 1;
}

int lua_engine_get_restitution(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!g_world->get_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.restitution));
  return 1;
}

int lua_engine_get_friction(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!g_world->get_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.staticFriction));
  lua_pushnumber(state, static_cast<lua_Number>(collider.dynamicFriction));
  return 2;
}

// --- MeshComponent: material getters/setters ---

int lua_engine_get_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.x));
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.y));
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.z));
  return 3;
}

int lua_engine_get_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(mesh->meshAssetId));
  return 1;
}

int lua_engine_set_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!g_world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.roughness = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

int lua_engine_get_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->roughness));
  return 1;
}

int lua_engine_set_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!g_world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.metallic = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

int lua_engine_get_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->metallic));
  return 1;
}

int lua_engine_set_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!g_world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.opacity = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

int lua_engine_get_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->opacity));
  return 1;
}

// --- LightComponent ---

int lua_engine_add_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *typeStr =
      lua_isstring(state, 2) ? lua_tostring(state, 2) : nullptr;
  runtime::LightComponent light{};
  if ((typeStr != nullptr) && (std::strcmp(typeStr, "point") == 0)) {
    light.type = runtime::LightType::Point;
  } else {
    light.type = runtime::LightType::Directional;
  }
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_remove_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_light_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_has_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, g_world->has_light_component(entity) ? 1 : 0);
  return 1;
}

int lua_engine_set_light_color(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 color{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &color)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!g_world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.color = color;
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_light_color(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::LightComponent light{};
  if (!g_world->get_light_component(entity, &light)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(light.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(light.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(light.color.z));
  return 3;
}

int lua_engine_set_light_intensity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!g_world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.intensity = static_cast<float>(lua_tonumber(state, 2));
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_get_light_intensity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::LightComponent light{};
  if (!g_world->get_light_component(entity, &light)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(light.intensity));
  return 1;
}

int lua_engine_set_light_direction(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 dir{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &dir)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!g_world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.direction = dir;
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- Collision handlers (registered, multi-listener) ---

static constexpr std::size_t kMaxCollisionHandlers = 8U;
static int g_collisionHandlers[kMaxCollisionHandlers] = {
    LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF,
    LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};

int lua_engine_on_collision_register(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
    if (g_collisionHandlers[i] == LUA_NOREF) {
      lua_pushvalue(state, 1);
      g_collisionHandlers[i] = luaL_ref(state, LUA_REGISTRYINDEX);
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }
  lua_pushnil(state);
  return 1;
}

int lua_engine_remove_collision_handler(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  const auto id = static_cast<std::size_t>(lua_tointeger(state, 1));
  if (id < kMaxCollisionHandlers) {
    if (g_collisionHandlers[id] != LUA_NOREF) {
      luaL_unref(state, LUA_REGISTRYINDEX, g_collisionHandlers[id]);
      g_collisionHandlers[id] = LUA_NOREF;
    }
  }
  return 0;
}

// --- Timer system ---

struct TimerEntry final {
  int luaRef = LUA_NOREF;
  float fireAt = 0.0F;
  float interval = 0.0F;
  bool repeat = false;
  bool active = false;
};

static constexpr std::size_t kMaxTimers = 64U;
static TimerEntry g_timers[kMaxTimers];

int lua_engine_set_timeout(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushnil(state);
    return 1;
  }
  const float secs = static_cast<float>(lua_tonumber(state, 2));
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!g_timers[i].active) {
      lua_pushvalue(state, 1);
      g_timers[i].luaRef = luaL_ref(state, LUA_REGISTRYINDEX);
      g_timers[i].fireAt = g_totalSeconds + secs;
      g_timers[i].interval = 0.0F;
      g_timers[i].repeat = false;
      g_timers[i].active = true;
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }
  lua_pushnil(state);
  return 1;
}

int lua_engine_set_interval(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushnil(state);
    return 1;
  }
  const float secs = static_cast<float>(lua_tonumber(state, 2));
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!g_timers[i].active) {
      lua_pushvalue(state, 1);
      g_timers[i].luaRef = luaL_ref(state, LUA_REGISTRYINDEX);
      g_timers[i].fireAt = g_totalSeconds + secs;
      g_timers[i].interval = secs;
      g_timers[i].repeat = true;
      g_timers[i].active = true;
      lua_pushinteger(state, static_cast<lua_Integer>(i));
      return 1;
    }
  }
  lua_pushnil(state);
  return 1;
}

int lua_engine_cancel_timer(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  const auto id = static_cast<std::size_t>(lua_tointeger(state, 1));
  if ((id < kMaxTimers) && g_timers[id].active) {
    luaL_unref(state, LUA_REGISTRYINDEX, g_timers[id].luaRef);
    g_timers[id].luaRef = LUA_NOREF;
    g_timers[id].active = false;
  }
  return 0;
}

// --- Coroutine scheduler ---

struct CoroutineEntry final {
  lua_State *thread = nullptr;
  float wakeAt = 0.0F;
  bool active = false;
};

static constexpr std::size_t kMaxCoroutines = 32U;
static CoroutineEntry g_coroutines[kMaxCoroutines];

int lua_engine_start_coroutine(lua_State *state) noexcept {
  if (!lua_isfunction(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  for (std::size_t i = 0U; i < kMaxCoroutines; ++i) {
    if (!g_coroutines[i].active) {
      lua_State *thread = lua_newthread(state);
      if (thread == nullptr) {
        lua_pushnil(state);
        return 1;
      }
      // Move the function onto the new thread's stack.
      lua_pushvalue(state, 1);
      lua_xmove(state, thread, 1);

      int nresults = 0;
      const int status = lua_resume(thread, state, 0, &nresults);
      if (status == LUA_OK) {
        // Coroutine finished immediately; no need to track it.
        lua_pushinteger(state, static_cast<lua_Integer>(i));
        return 1;
      }
      if (status == LUA_YIELD) {
        // Coroutine yielded; check if it passed a sleep duration.
        float wakeAt = g_totalSeconds; // default: wake next tick
        if (nresults >= 1) {
          if (lua_isnumber(thread, -1)) {
            const float secs = static_cast<float>(lua_tonumber(thread, -1));
            wakeAt = g_totalSeconds + secs;
          }
          lua_pop(thread, nresults);
        }
        g_coroutines[i].thread = thread;
        g_coroutines[i].wakeAt = wakeAt;
        g_coroutines[i].active = true;
        lua_pushinteger(state, static_cast<lua_Integer>(i));
        return 1;
      }
      // Error: log and discard.
      log_lua_error("start_coroutine");
      lua_pushnil(state);
      return 1;
    }
  }
  lua_pushnil(state);
  return 1;
}

int lua_engine_wait(lua_State *state) noexcept {
  // Just yield with the sleep duration as the result so the scheduler
  // can capture it in tick_coroutines().
  const float secs = lua_isnumber(state, 1)
                         ? static_cast<float>(lua_tonumber(state, 1))
                         : 0.0F;
  lua_pushnumber(state, static_cast<lua_Number>(secs));
  return lua_yield(state, 1);
}

// --- Entity lifecycle completeness ---

int lua_engine_find_by_name(lua_State *state) noexcept {
  if (g_world == nullptr || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *searchName = lua_tostring(state, 1);
  if (searchName == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity found = g_world->find_entity_by_name(searchName);

  if (found == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(found.index));
  return 1;
}

int lua_engine_clone_entity(lua_State *state) noexcept {
  if ((g_world == nullptr) || !can_apply_mutations_now()) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity source{};
  if (!read_entity(state, 1, &source)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity newEntity = g_world->create_entity();
  if (newEntity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  // Copy Transform.
  runtime::Transform transform{};
  if (g_world->get_transform(source, &transform)) {
    static_cast<void>(g_world->add_transform(newEntity, transform));
  }

  // Copy RigidBody.
  runtime::RigidBody rigidBody{};
  if (g_world->get_rigid_body(source, &rigidBody)) {
    static_cast<void>(g_world->add_rigid_body(newEntity, rigidBody));
  }

  // Copy Collider.
  runtime::Collider collider{};
  if (g_world->get_collider(source, &collider)) {
    static_cast<void>(g_world->add_collider(newEntity, collider));
  }

  // Copy MeshComponent.
  runtime::MeshComponent mesh{};
  if (g_world->get_mesh_component(source, &mesh)) {
    static_cast<void>(g_world->add_mesh_component(newEntity, mesh));
  }

  // Copy NameComponent with "(clone)" suffix.
  runtime::NameComponent name{};
  if (g_world->get_name_component(source, &name)) {
    runtime::NameComponent cloneName{};
    copy_clone_name(cloneName.name, sizeof(cloneName.name), name.name);
    static_cast<void>(g_world->add_name_component(newEntity, cloneName));
  }

  // Copy LightComponent.
  runtime::LightComponent light{};
  if (g_world->get_light_component(source, &light)) {
    static_cast<void>(g_world->add_light_component(newEntity, light));
  }

  lua_pushinteger(state, static_cast<lua_Integer>(newEntity.index));
  return 1;
}

// --- Scene management from Lua (deferred load/new, immediate save) ---

enum class SceneOp : std::uint8_t { None, Load, New };
static SceneOp g_pendingSceneOp = SceneOp::None;
static char g_pendingScenePath[512] = {};

int lua_engine_save_scene(lua_State *state) noexcept {
  if (g_world == nullptr || !lua_isstring(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = (g_services != nullptr) && (g_services->save_scene != nullptr)
                      ? g_services->save_scene(g_world, path)
                      : false;
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

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

int lua_engine_new_scene(lua_State *state) noexcept {
  static_cast<void>(state);
  g_pendingSceneOp = SceneOp::New;
  return 0;
}

// --- Prefab bindings ---

int lua_engine_save_prefab(lua_State *state) noexcept {
  if ((g_world == nullptr) || !lua_isinteger(state, 1) ||
      !lua_isstring(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 2);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok =
      (g_services != nullptr) && (g_services->save_prefab != nullptr)
          ? g_services->save_prefab(g_world, entity.index, path)
          : false;
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_instantiate(lua_State *state) noexcept {
  if ((g_world == nullptr) || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  const std::uint32_t entityIndex =
      ((g_services != nullptr) && (g_services->instantiate_prefab != nullptr))
          ? g_services->instantiate_prefab(g_world, path)
          : 0U;
  if (entityIndex == 0U) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(entityIndex));
  return 1;
}

// --- Entity pool Lua bindings ---

// engine.pool_create(count) → pool_id or nil
int lua_engine_pool_create(lua_State *state) noexcept {
  if ((g_world == nullptr) || !lua_isinteger(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer count = lua_tointeger(state, 1);
  if ((count <= 0) ||
      (static_cast<std::size_t>(count) > runtime::EntityPool::kMaxPoolSize)) {
    lua_pushnil(state);
    return 1;
  }

  if (g_entityPoolCount >= kMaxEntityPools) {
    lua_pushnil(state);
    return 1;
  }

  runtime::EntityPool &pool = g_entityPools[g_entityPoolCount];
  if (!pool.init(g_world, static_cast<std::size_t>(count))) {
    lua_pushnil(state);
    return 1;
  }

  const std::size_t poolId = g_entityPoolCount;
  ++g_entityPoolCount;
  lua_pushinteger(state, static_cast<lua_Integer>(poolId));
  return 1;
}

// engine.pool_spawn(pool_id) → entity_index or nil
int lua_engine_pool_spawn(lua_State *state) noexcept {
  if (!lua_isinteger(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer poolId = lua_tointeger(state, 1);
  if ((poolId < 0) || (static_cast<std::size_t>(poolId) >= g_entityPoolCount)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity entity =
      g_entityPools[static_cast<std::size_t>(poolId)].acquire();
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(entity.index));
  return 1;
}

// engine.pool_release(pool_id, entity_index) → bool
int lua_engine_pool_release(lua_State *state) noexcept {
  if (!lua_isinteger(state, 1) || !lua_isinteger(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer poolId = lua_tointeger(state, 1);
  if ((poolId < 0) || (static_cast<std::size_t>(poolId) >= g_entityPoolCount)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity{};
  if (!read_entity(state, 2, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const bool ok =
      g_entityPools[static_cast<std::size_t>(poolId)].release(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Find an already-loaded entity script module by path, or load it fresh.
// Returns LUA_NOREF on failure. Must be called after log_lua_error is defined.
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
      const std::int64_t currentMtime = get_file_mtime(path);
      if ((currentMtime != 0) && (mod.mtime != 0) &&
          (currentMtime != mod.mtime)) {
        if (luaL_loadfile(g_state, path) != LUA_OK) {
          log_lua_error("reload entity script");
          return mod.registryRef;
        }

        if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
          log_lua_error("reload entity script");
          return mod.registryRef;
        }

        if (!lua_istable(g_state, -1)) {
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

  if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
    log_lua_error("exec entity script");
    --g_moduleLoadDepth;
    return LUA_NOREF;
  }

  if (!lua_istable(g_state, -1)) {
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
  mod.mtime = get_file_mtime(path);
  mod.reloaded = false;
  ++g_entityScriptModuleCount;

  char logBuf[256] = {};
  std::snprintf(logBuf, sizeof(logBuf), "loaded entity script: %s", path);
  core::log_message(core::LogLevel::Info, "scripting", logBuf);
  --g_moduleLoadDepth;
  return ref;
}

// Call module.funcName(entityIndex [, dt]) — returns false on missing/error.
// Call a function on a module table, with optional fallback name.
// If entityIndex > 0, marks entity as faulted on error.
bool call_module_function(int moduleRef, const char *funcName,
                          const char *fallbackName, std::uint32_t entityIndex,
                          bool hasDt, float dt) noexcept {
  if ((g_state == nullptr) || (moduleRef == LUA_NOREF)) {
    return false;
  }

  lua_rawgeti(g_state, LUA_REGISTRYINDEX, moduleRef);
  if (!lua_istable(g_state, -1)) {
    lua_pop(g_state, 1);
    return false;
  }

  lua_getfield(g_state, -1, funcName);
  if (!lua_isfunction(g_state, -1)) {
    lua_pop(g_state, 1);
    // Try fallback name if provided.
    if (fallbackName != nullptr) {
      lua_getfield(g_state, -1, fallbackName);
      if (!lua_isfunction(g_state, -1)) {
        lua_pop(g_state, 2);
        return false;
      }
    } else {
      lua_pop(g_state, 1);
      return false;
    }
  }

  // Stack: ... | table | func
  // Remove table so it doesn't interfere with the pcall argument count.
  lua_remove(g_state, -2);

  lua_pushinteger(g_state, static_cast<lua_Integer>(entityIndex));
  int nargs = 1;
  if (hasDt) {
    lua_pushnumber(g_state, static_cast<lua_Number>(dt));
    nargs = 2;
  }

  if (lua_pcall(g_state, nargs, 0, 0) != LUA_OK) {
    log_lua_error(funcName);
    if ((entityIndex > 0U) && (entityIndex < kMaxFaultedEntities)) {
      g_entityFaulted[entityIndex] = true;
    }
    return false;
  }
  return true;
}

int lua_engine_add_script_component(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const char *path = lua_tostring(state, 2);
  if ((path == nullptr) || (path[0] == '\0')) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::ScriptComponent comp{};
  const std::size_t maxLen = sizeof(comp.scriptPath) - 1U;
  const std::size_t len = std::strlen(path);
  const std::size_t copy = (len > maxLen) ? maxLen : len;
  std::memcpy(comp.scriptPath, path, copy);
  comp.scriptPath[copy] = '\0';

  lua_pushboolean(state, apply_or_queue_script_component(entity, comp) ? 1 : 0);
  return 1;
}

int lua_engine_remove_script_component(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state,
                  apply_or_queue_remove_script_component(entity) ? 1 : 0);
  return 1;
}

// engine.require(path) — load a Lua module file and return its table.
// The module is cached by path (same cache used by entity scripts).
// Suitable for shared utility scripts that don't need entity lifecycle hooks.
// Returns nil on failure.
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

void register_engine_bindings(lua_State *state) noexcept {
  lua_newtable(state);

  lua_pushcfunction(state, &lua_engine_log);
  lua_setfield(state, -2, "log");

  lua_pushcfunction(state, &lua_engine_get_entity_count);
  lua_setfield(state, -2, "get_entity_count");

  lua_pushcfunction(state, &lua_engine_spawn_entity);
  lua_setfield(state, -2, "spawn_entity");

  lua_pushcfunction(state, &lua_engine_destroy_entity);
  lua_setfield(state, -2, "destroy_entity");

  lua_pushcfunction(state, &lua_engine_is_alive);
  lua_setfield(state, -2, "is_alive");

  lua_pushcfunction(state, &lua_engine_get_position);
  lua_setfield(state, -2, "get_position");

  lua_pushcfunction(state, &lua_engine_set_position);
  lua_setfield(state, -2, "set_position");

  lua_pushcfunction(state, &lua_engine_get_velocity);
  lua_setfield(state, -2, "get_velocity");

  lua_pushcfunction(state, &lua_engine_add_rigid_body);
  lua_setfield(state, -2, "add_rigid_body");

  lua_pushcfunction(state, &lua_engine_set_velocity);
  lua_setfield(state, -2, "set_velocity");

  lua_pushcfunction(state, &lua_engine_set_acceleration);
  lua_setfield(state, -2, "set_acceleration");

  lua_pushcfunction(state, &lua_engine_set_additional_acceleration);
  lua_setfield(state, -2, "set_additional_acceleration");

  lua_pushcfunction(state, &lua_engine_get_angular_velocity);
  lua_setfield(state, -2, "get_angular_velocity");

  lua_pushcfunction(state, &lua_engine_set_angular_velocity);
  lua_setfield(state, -2, "set_angular_velocity");

  lua_pushcfunction(state, &lua_engine_set_mesh);
  lua_setfield(state, -2, "set_mesh");

  lua_pushcfunction(state, &lua_engine_get_default_mesh_asset_id);
  lua_setfield(state, -2, "get_default_mesh_asset_id");

  lua_pushcfunction(state, &lua_engine_spawn_shape);
  lua_setfield(state, -2, "spawn_shape");

  lua_pushcfunction(state, &lua_engine_set_albedo);
  lua_setfield(state, -2, "set_albedo");

  lua_pushcfunction(state, &lua_engine_set_name);
  lua_setfield(state, -2, "set_name");

  lua_pushcfunction(state, &lua_engine_get_name);
  lua_setfield(state, -2, "get_name");

  lua_pushcfunction(state, &lua_engine_add_collider);
  lua_setfield(state, -2, "add_collider");

  lua_pushcfunction(state, &lua_engine_set_restitution);
  lua_setfield(state, -2, "set_restitution");

  lua_pushcfunction(state, &lua_engine_set_friction);
  lua_setfield(state, -2, "set_friction");

  lua_pushcfunction(state, &lua_engine_delta_time);
  lua_setfield(state, -2, "delta_time");

  lua_pushcfunction(state, &lua_engine_elapsed_time);
  lua_setfield(state, -2, "elapsed_time");

  lua_pushcfunction(state, &lua_engine_is_key_down);
  lua_setfield(state, -2, "is_key_down");

  lua_pushcfunction(state, &lua_engine_is_key_pressed);
  lua_setfield(state, -2, "is_key_pressed");

  lua_pushcfunction(state, &lua_engine_register_action);
  lua_setfield(state, -2, "register_action");
  lua_pushcfunction(state, &lua_engine_register_axis);
  lua_setfield(state, -2, "register_axis");
  lua_pushcfunction(state, &lua_engine_is_action_down);
  lua_setfield(state, -2, "is_action_down");
  lua_pushcfunction(state, &lua_engine_is_action_pressed);
  lua_setfield(state, -2, "is_action_pressed");
  lua_pushcfunction(state, &lua_engine_get_action_value);
  lua_setfield(state, -2, "action_value");
  lua_pushcfunction(state, &lua_engine_get_axis_value);
  lua_setfield(state, -2, "axis_value");

  lua_pushcfunction(state, &lua_engine_is_gamepad_connected);
  lua_setfield(state, -2, "is_gamepad_connected");
  lua_pushcfunction(state, &lua_engine_is_gamepad_button_down);
  lua_setfield(state, -2, "is_gamepad_button_down");
  lua_pushcfunction(state, &lua_engine_gamepad_axis_value);
  lua_setfield(state, -2, "gamepad_axis_value");

  // InputMapper bindings (P1-M2-C).
  lua_pushcfunction(state, &lua_engine_add_input_action);
  lua_setfield(state, -2, "add_input_action");
  lua_pushcfunction(state, &lua_engine_add_input_axis);
  lua_setfield(state, -2, "add_input_axis");
  lua_pushcfunction(state, &lua_engine_is_mapped_action_down);
  lua_setfield(state, -2, "is_mapped_action_down");
  lua_pushcfunction(state, &lua_engine_is_mapped_action_pressed);
  lua_setfield(state, -2, "is_mapped_action_pressed");
  lua_pushcfunction(state, &lua_engine_mapped_axis_value);
  lua_setfield(state, -2, "mapped_axis_value");
  lua_pushcfunction(state, &lua_engine_rebind_action);
  lua_setfield(state, -2, "rebind_action");
  lua_pushcfunction(state, &lua_engine_save_input_config);
  lua_setfield(state, -2, "save_input_config");
  lua_pushcfunction(state, &lua_engine_load_input_config);
  lua_setfield(state, -2, "load_input_config");

  // Touch/gesture bindings (P1-M2-C3e).
  lua_pushcfunction(state, &lua_engine_on_touch);
  lua_setfield(state, -2, "on_touch");
  lua_pushcfunction(state, &lua_engine_on_gesture);
  lua_setfield(state, -2, "on_gesture");
  lua_pushcfunction(state, &lua_engine_set_touch_mouse_emulation);
  lua_setfield(state, -2, "set_touch_mouse_emulation");

  lua_pushcfunction(state, &lua_engine_set_game_mode);
  lua_setfield(state, -2, "set_game_mode");
  lua_pushcfunction(state, &lua_engine_get_game_mode);
  lua_setfield(state, -2, "get_game_mode");
  lua_pushcfunction(state, &lua_engine_set_game_state);
  lua_setfield(state, -2, "set_game_state");
  lua_pushcfunction(state, &lua_engine_get_game_state);
  lua_setfield(state, -2, "get_game_state");
  lua_pushcfunction(state, &lua_engine_set_player_controller);
  lua_setfield(state, -2, "set_player_controller");
  lua_pushcfunction(state, &lua_engine_get_player_controller);
  lua_setfield(state, -2, "get_player_controller");

  // Game mode state transitions and rules.
  lua_pushcfunction(state, &lua_engine_game_mode_start);
  lua_setfield(state, -2, "game_mode_start");
  lua_pushcfunction(state, &lua_engine_game_mode_pause);
  lua_setfield(state, -2, "game_mode_pause");
  lua_pushcfunction(state, &lua_engine_game_mode_end);
  lua_setfield(state, -2, "game_mode_end");
  lua_pushcfunction(state, &lua_engine_game_mode_state);
  lua_setfield(state, -2, "game_mode_state");
  lua_pushcfunction(state, &lua_engine_game_mode_set_rule);
  lua_setfield(state, -2, "game_mode_set_rule");
  lua_pushcfunction(state, &lua_engine_game_mode_get_rule);
  lua_setfield(state, -2, "game_mode_get_rule");
  lua_pushcfunction(state, &lua_engine_game_mode_max_players);
  lua_setfield(state, -2, "game_mode_max_players");

  // Persistent game state (survives scene transitions).
  lua_pushcfunction(state, &lua_engine_game_state_set_number);
  lua_setfield(state, -2, "game_state_set_number");
  lua_pushcfunction(state, &lua_engine_game_state_get_number);
  lua_setfield(state, -2, "game_state_get_number");
  lua_pushcfunction(state, &lua_engine_game_state_set_string);
  lua_setfield(state, -2, "game_state_set_string");
  lua_pushcfunction(state, &lua_engine_game_state_get_string);
  lua_setfield(state, -2, "game_state_get_string");
  lua_pushcfunction(state, &lua_engine_game_state_has);
  lua_setfield(state, -2, "game_state_has");
  lua_pushcfunction(state, &lua_engine_game_state_clear);
  lua_setfield(state, -2, "game_state_clear");

  lua_pushcfunction(state, &lua_engine_is_god_mode);
  lua_setfield(state, -2, "is_god_mode");
  lua_pushcfunction(state, &lua_engine_is_noclip);
  lua_setfield(state, -2, "is_noclip");

  lua_pushcfunction(state, &lua_engine_profiler_enable);
  lua_setfield(state, -2, "profiler_enable");
  lua_pushcfunction(state, &lua_engine_profiler_reset);
  lua_setfield(state, -2, "profiler_reset");
  lua_pushcfunction(state, &lua_engine_profiler_get_count);
  lua_setfield(state, -2, "profiler_get_count");

  lua_pushcfunction(state, &lua_engine_debugger_enable);
  lua_setfield(state, -2, "debugger_enable");
  lua_pushcfunction(state, &lua_engine_debugger_add_breakpoint);
  lua_setfield(state, -2, "debugger_add_breakpoint");
  lua_pushcfunction(state, &lua_engine_debugger_clear_breakpoints);
  lua_setfield(state, -2, "debugger_clear_breakpoints");
  lua_pushcfunction(state, &lua_engine_debugger_add_watch);
  lua_setfield(state, -2, "debugger_add_watch");
  lua_pushcfunction(state, &lua_engine_debugger_clear_watches);
  lua_setfield(state, -2, "debugger_clear_watches");
  lua_pushcfunction(state, &lua_engine_debugger_last_breakpoint);
  lua_setfield(state, -2, "debugger_last_breakpoint");
  lua_pushcfunction(state, &lua_engine_debugger_last_callstack);
  lua_setfield(state, -2, "debugger_last_callstack");
  lua_pushcfunction(state, &lua_engine_debugger_last_watch_values);
  lua_setfield(state, -2, "debugger_last_watch_values");

  lua_pushcfunction(state, &lua_engine_set_camera_position);
  lua_setfield(state, -2, "set_camera_position");

  lua_pushcfunction(state, &lua_engine_set_camera_target);
  lua_setfield(state, -2, "set_camera_target");

  lua_pushcfunction(state, &lua_engine_set_camera_up);
  lua_setfield(state, -2, "set_camera_up");

  lua_pushcfunction(state, &lua_engine_set_camera_fov);
  lua_setfield(state, -2, "set_camera_fov");

  lua_pushcfunction(state, &lua_engine_set_gravity);
  lua_setfield(state, -2, "set_gravity");

  lua_pushcfunction(state, &lua_engine_get_gravity);
  lua_setfield(state, -2, "get_gravity");

  lua_pushcfunction(state, &lua_engine_raycast);
  lua_setfield(state, -2, "raycast");

  lua_pushcfunction(state, &lua_engine_add_distance_joint);
  lua_setfield(state, -2, "add_distance_joint");

  lua_pushcfunction(state, &lua_engine_remove_joint);
  lua_setfield(state, -2, "remove_joint");

  lua_pushcfunction(state, &lua_engine_wake_body);
  lua_setfield(state, -2, "wake_body");

  lua_pushcfunction(state, &lua_engine_is_sleeping);
  lua_setfield(state, -2, "is_sleeping");

  lua_pushcfunction(state, &lua_engine_frame_count);
  lua_setfield(state, -2, "frame_count");

  lua_pushcfunction(state, &lua_engine_load_sound);
  lua_setfield(state, -2, "load_sound");

  lua_pushcfunction(state, &lua_engine_unload_sound);
  lua_setfield(state, -2, "unload_sound");

  lua_pushcfunction(state, &lua_engine_play_sound);
  lua_setfield(state, -2, "play_sound");

  lua_pushcfunction(state, &lua_engine_stop_sound);
  lua_setfield(state, -2, "stop_sound");

  lua_pushcfunction(state, &lua_engine_stop_all_sounds);
  lua_setfield(state, -2, "stop_all_sounds");

  lua_pushcfunction(state, &lua_engine_set_master_volume);
  lua_setfield(state, -2, "set_master_volume");

  // Key scancode constants (values match SDL_SCANCODE_* for the SDL2 backend).
  lua_pushinteger(state, core::kKey_A);
  lua_setfield(state, -2, "KEY_A");
  lua_pushinteger(state, core::kKey_B);
  lua_setfield(state, -2, "KEY_B");
  lua_pushinteger(state, core::kKey_C);
  lua_setfield(state, -2, "KEY_C");
  lua_pushinteger(state, core::kKey_D);
  lua_setfield(state, -2, "KEY_D");
  lua_pushinteger(state, core::kKey_E);
  lua_setfield(state, -2, "KEY_E");
  lua_pushinteger(state, core::kKey_F);
  lua_setfield(state, -2, "KEY_F");
  lua_pushinteger(state, core::kKey_G);
  lua_setfield(state, -2, "KEY_G");
  lua_pushinteger(state, core::kKey_H);
  lua_setfield(state, -2, "KEY_H");
  lua_pushinteger(state, core::kKey_I);
  lua_setfield(state, -2, "KEY_I");
  lua_pushinteger(state, core::kKey_J);
  lua_setfield(state, -2, "KEY_J");
  lua_pushinteger(state, core::kKey_K);
  lua_setfield(state, -2, "KEY_K");
  lua_pushinteger(state, core::kKey_L);
  lua_setfield(state, -2, "KEY_L");
  lua_pushinteger(state, core::kKey_M);
  lua_setfield(state, -2, "KEY_M");
  lua_pushinteger(state, core::kKey_N);
  lua_setfield(state, -2, "KEY_N");
  lua_pushinteger(state, core::kKey_O);
  lua_setfield(state, -2, "KEY_O");
  lua_pushinteger(state, core::kKey_P);
  lua_setfield(state, -2, "KEY_P");
  lua_pushinteger(state, core::kKey_Q);
  lua_setfield(state, -2, "KEY_Q");
  lua_pushinteger(state, core::kKey_R);
  lua_setfield(state, -2, "KEY_R");
  lua_pushinteger(state, core::kKey_S);
  lua_setfield(state, -2, "KEY_S");
  lua_pushinteger(state, core::kKey_T);
  lua_setfield(state, -2, "KEY_T");
  lua_pushinteger(state, core::kKey_U);
  lua_setfield(state, -2, "KEY_U");
  lua_pushinteger(state, core::kKey_V);
  lua_setfield(state, -2, "KEY_V");
  lua_pushinteger(state, core::kKey_W);
  lua_setfield(state, -2, "KEY_W");
  lua_pushinteger(state, core::kKey_X);
  lua_setfield(state, -2, "KEY_X");
  lua_pushinteger(state, core::kKey_Y);
  lua_setfield(state, -2, "KEY_Y");
  lua_pushinteger(state, core::kKey_Z);
  lua_setfield(state, -2, "KEY_Z");
  lua_pushinteger(state, core::kKey_0);
  lua_setfield(state, -2, "KEY_0");
  lua_pushinteger(state, core::kKey_1);
  lua_setfield(state, -2, "KEY_1");
  lua_pushinteger(state, core::kKey_2);
  lua_setfield(state, -2, "KEY_2");
  lua_pushinteger(state, core::kKey_3);
  lua_setfield(state, -2, "KEY_3");
  lua_pushinteger(state, core::kKey_4);
  lua_setfield(state, -2, "KEY_4");
  lua_pushinteger(state, core::kKey_5);
  lua_setfield(state, -2, "KEY_5");
  lua_pushinteger(state, core::kKey_6);
  lua_setfield(state, -2, "KEY_6");
  lua_pushinteger(state, core::kKey_7);
  lua_setfield(state, -2, "KEY_7");
  lua_pushinteger(state, core::kKey_8);
  lua_setfield(state, -2, "KEY_8");
  lua_pushinteger(state, core::kKey_9);
  lua_setfield(state, -2, "KEY_9");
  lua_pushinteger(state, core::kKey_Space);
  lua_setfield(state, -2, "KEY_SPACE");
  lua_pushinteger(state, core::kKey_Return);
  lua_setfield(state, -2, "KEY_RETURN");
  lua_pushinteger(state, core::kKey_Escape);
  lua_setfield(state, -2, "KEY_ESCAPE");
  lua_pushinteger(state, core::kKey_Up);
  lua_setfield(state, -2, "KEY_UP");
  lua_pushinteger(state, core::kKey_Down);
  lua_setfield(state, -2, "KEY_DOWN");
  lua_pushinteger(state, core::kKey_Left);
  lua_setfield(state, -2, "KEY_LEFT");
  lua_pushinteger(state, core::kKey_Right);
  lua_setfield(state, -2, "KEY_RIGHT");
  lua_pushinteger(state, core::kKey_LShift);
  lua_setfield(state, -2, "KEY_LSHIFT");
  lua_pushinteger(state, core::kKey_LCtrl);
  lua_setfield(state, -2, "KEY_LCTRL");
  lua_pushinteger(state, core::kKey_LAlt);
  lua_setfield(state, -2, "KEY_LALT");

  // Transform: rotation and scale
  lua_pushcfunction(state, &lua_engine_get_rotation);
  lua_setfield(state, -2, "get_rotation");
  lua_pushcfunction(state, &lua_engine_set_rotation);
  lua_setfield(state, -2, "set_rotation");
  lua_pushcfunction(state, &lua_engine_get_scale);
  lua_setfield(state, -2, "get_scale");
  lua_pushcfunction(state, &lua_engine_set_scale);
  lua_setfield(state, -2, "set_scale");

  // RigidBody: inverse mass
  lua_pushcfunction(state, &lua_engine_get_inverse_mass);
  lua_setfield(state, -2, "get_inverse_mass");
  lua_pushcfunction(state, &lua_engine_set_inverse_mass);
  lua_setfield(state, -2, "set_inverse_mass");

  // Collider: getters
  lua_pushcfunction(state, &lua_engine_get_half_extents);
  lua_setfield(state, -2, "get_half_extents");
  lua_pushcfunction(state, &lua_engine_set_half_extents);
  lua_setfield(state, -2, "set_half_extents");
  lua_pushcfunction(state, &lua_engine_get_restitution);
  lua_setfield(state, -2, "get_restitution");
  lua_pushcfunction(state, &lua_engine_get_friction);
  lua_setfield(state, -2, "get_friction");

  // MeshComponent: material
  lua_pushcfunction(state, &lua_engine_get_albedo);
  lua_setfield(state, -2, "get_albedo");
  lua_pushcfunction(state, &lua_engine_get_mesh);
  lua_setfield(state, -2, "get_mesh");
  lua_pushcfunction(state, &lua_engine_set_roughness);
  lua_setfield(state, -2, "set_roughness");
  lua_pushcfunction(state, &lua_engine_get_roughness);
  lua_setfield(state, -2, "get_roughness");
  lua_pushcfunction(state, &lua_engine_set_metallic);
  lua_setfield(state, -2, "set_metallic");
  lua_pushcfunction(state, &lua_engine_get_metallic);
  lua_setfield(state, -2, "get_metallic");
  lua_pushcfunction(state, &lua_engine_set_opacity);
  lua_setfield(state, -2, "set_opacity");
  lua_pushcfunction(state, &lua_engine_get_opacity);
  lua_setfield(state, -2, "get_opacity");

  // LightComponent
  lua_pushcfunction(state, &lua_engine_add_light);
  lua_setfield(state, -2, "add_light");
  lua_pushcfunction(state, &lua_engine_remove_light);
  lua_setfield(state, -2, "remove_light");
  lua_pushcfunction(state, &lua_engine_has_light);
  lua_setfield(state, -2, "has_light");
  lua_pushcfunction(state, &lua_engine_set_light_color);
  lua_setfield(state, -2, "set_light_color");
  lua_pushcfunction(state, &lua_engine_get_light_color);
  lua_setfield(state, -2, "get_light_color");
  lua_pushcfunction(state, &lua_engine_set_light_intensity);
  lua_setfield(state, -2, "set_light_intensity");
  lua_pushcfunction(state, &lua_engine_get_light_intensity);
  lua_setfield(state, -2, "get_light_intensity");
  lua_pushcfunction(state, &lua_engine_set_light_direction);
  lua_setfield(state, -2, "set_light_direction");

  // Collision handlers
  lua_pushcfunction(state, &lua_engine_on_collision_register);
  lua_setfield(state, -2, "on_collision_handler");
  lua_pushcfunction(state, &lua_engine_remove_collision_handler);
  lua_setfield(state, -2, "remove_collision_handler");

  // Timers
  lua_pushcfunction(state, &lua_engine_set_timeout);
  lua_setfield(state, -2, "set_timeout");
  lua_pushcfunction(state, &lua_engine_set_interval);
  lua_setfield(state, -2, "set_interval");
  lua_pushcfunction(state, &lua_engine_cancel_timer);
  lua_setfield(state, -2, "cancel_timer");

  // Coroutines
  lua_pushcfunction(state, &lua_engine_start_coroutine);
  lua_setfield(state, -2, "start_coroutine");
  lua_pushcfunction(state, &lua_engine_wait);
  lua_setfield(state, -2, "wait");

  // Entity lifecycle completeness
  lua_pushcfunction(state, &lua_engine_find_by_name);
  lua_setfield(state, -2, "find_entity_by_name");
  lua_pushcfunction(state, &lua_engine_clone_entity);
  lua_setfield(state, -2, "clone_entity");

  // Scene management
  lua_pushcfunction(state, &lua_engine_save_scene);
  lua_setfield(state, -2, "save_scene");
  lua_pushcfunction(state, &lua_engine_load_scene);
  lua_setfield(state, -2, "load_scene");
  lua_pushcfunction(state, &lua_engine_new_scene);
  lua_setfield(state, -2, "new_scene");

  // Prefab system
  lua_pushcfunction(state, &lua_engine_save_prefab);
  lua_setfield(state, -2, "save_prefab");
  lua_pushcfunction(state, &lua_engine_instantiate);
  lua_setfield(state, -2, "instantiate");

  // Entity pooling
  lua_pushcfunction(state, &lua_engine_pool_create);
  lua_setfield(state, -2, "pool_create");
  lua_pushcfunction(state, &lua_engine_pool_spawn);
  lua_setfield(state, -2, "pool_spawn");
  lua_pushcfunction(state, &lua_engine_pool_release);
  lua_setfield(state, -2, "pool_release");

  // Per-entity scripts (ScriptComponent)
  lua_pushcfunction(state, &lua_engine_add_script_component);
  lua_setfield(state, -2, "add_script_component");
  lua_pushcfunction(state, &lua_engine_remove_script_component);
  lua_setfield(state, -2, "remove_script_component");

  // Utility module loader — load a Lua file as a shared module (cached).
  lua_pushcfunction(state, &lua_engine_require);
  lua_setfield(state, -2, "require");

  lua_setglobal(state, "engine");
}

// ---- Console cheat commands ----

void cmd_god(const char *const * /*args*/, int /*argCount*/,
             void * /*userData*/) noexcept {
  g_godModeEnabled = !g_godModeEnabled;
  core::console_print(g_godModeEnabled ? "God mode ON" : "God mode OFF");
}

void cmd_noclip(const char *const * /*args*/, int /*argCount*/,
                void * /*userData*/) noexcept {
  g_noclipEnabled = !g_noclipEnabled;
  core::console_print(g_noclipEnabled ? "Noclip ON" : "Noclip OFF");
}

void cmd_spawn(const char *const *args, int argCount,
               void * /*userData*/) noexcept {
  if (argCount < 2) {
    core::console_print("Usage: spawn <prefab> [x y z]");
    return;
  }
  if ((g_world == nullptr) || (g_services == nullptr) ||
      (g_services->instantiate_prefab == nullptr)) {
    core::console_print("Cannot spawn: world not ready");
    return;
  }
  const std::uint32_t entityIndex =
      g_services->instantiate_prefab(g_world, args[1]);
  if (entityIndex == 0U) {
    core::console_print("Spawn failed (prefab not found?)");
    return;
  }
  // Optionally set position if x y z provided.
  if ((argCount >= 5) && (g_services->add_transform_op != nullptr)) {
    runtime::Transform t{};
    t.position.x = static_cast<float>(std::atof(args[2]));
    t.position.y = static_cast<float>(std::atof(args[3]));
    t.position.z = static_cast<float>(std::atof(args[4]));
    t.scale = {1.0F, 1.0F, 1.0F};
    t.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    // Overwrite the transform that was loaded from the prefab.
    g_services->add_transform_op(g_world, entityIndex, t);
  }
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "Spawned entity %u", entityIndex);
  core::console_print(buf);
}

void cmd_kill_all(const char *const * /*args*/, int /*argCount*/,
                  void * /*userData*/) noexcept {
  if ((g_world == nullptr) || (g_services == nullptr) ||
      (g_services->destroy_entity_op == nullptr)) {
    core::console_print("Cannot kill_all: world not ready");
    return;
  }
  std::size_t destroyed = 0U;
  g_world->for_each_alive([&destroyed](runtime::Entity entity) noexcept {
    // Skip player controller entities.
    for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
      if (g_playerControllerEntities[i] == entity.index) {
        return;
      }
      if (g_playerControllers.get_controlled_entity(
              static_cast<std::uint8_t>(i)) == entity.index) {
        return;
      }
    }
    g_services->destroy_entity_op(g_world, entity.index);
    ++destroyed;
  });
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "Destroyed %zu entities", destroyed);
  core::console_print(buf);
}

void register_debug_commands() noexcept {
  core::console_register_command("god", cmd_god, nullptr,
                                 "Toggle god mode (invincibility)");
  core::console_register_command("noclip", cmd_noclip, nullptr,
                                 "Toggle noclip (no collision)");
  core::console_register_command("spawn", cmd_spawn, nullptr,
                                 "Spawn a prefab: spawn <path> [x y z]");
  core::console_register_command("kill_all", cmd_kill_all, nullptr,
                                 "Destroy all entities except player");
}

} // namespace

bool initialize_scripting() noexcept {
  if (g_state != nullptr) {
    return true;
  }

  g_state = luaL_newstate();
  if (g_state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "failed to create Lua state");
    return false;
  }

  // Open only safe libraries. io, os, debug, and package are excluded to
  // prevent untrusted game scripts from accessing the file system or executing
  // arbitrary system commands.
  luaL_requiref(g_state, LUA_GNAME, luaopen_base, 1);
  lua_pop(g_state, 1);
  luaL_requiref(g_state, LUA_COLIBNAME, luaopen_coroutine, 1);
  lua_pop(g_state, 1);
  luaL_requiref(g_state, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(g_state, 1);
  luaL_requiref(g_state, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(g_state, 1);
  luaL_requiref(g_state, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(g_state, 1);
  luaL_requiref(g_state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  lua_pop(g_state, 1);
  register_engine_bindings(g_state);
  register_debug_commands();
  refresh_lua_hook();
  return true;
}

void shutdown_scripting() noexcept {
  if (g_state != nullptr) {
    // Release all timer refs before closing.
    for (std::size_t i = 0U; i < kMaxTimers; ++i) {
      if (g_timers[i].active && (g_timers[i].luaRef != LUA_NOREF)) {
        luaL_unref(g_state, LUA_REGISTRYINDEX, g_timers[i].luaRef);
      }
      g_timers[i] = TimerEntry{};
    }
    // Release collision handler refs.
    for (std::size_t i = 0U; i < kMaxCollisionHandlers; ++i) {
      if (g_collisionHandlers[i] != LUA_NOREF) {
        luaL_unref(g_state, LUA_REGISTRYINDEX, g_collisionHandlers[i]);
        g_collisionHandlers[i] = LUA_NOREF;
      }
    }
    // Mark all coroutines inactive (GC handles thread objects).
    for (std::size_t i = 0U; i < kMaxCoroutines; ++i) {
      g_coroutines[i] = CoroutineEntry{};
    }
    lua_close(g_state);
    g_state = nullptr;
  }

  g_world = nullptr;
  g_defaultMeshAssetId = 0U;
  g_builtinPlaneMesh = 0U;
  g_builtinCubeMesh = 0U;
  g_builtinSphereMesh = 0U;
  g_builtinCylinderMesh = 0U;
  g_builtinCapsuleMesh = 0U;
  g_builtinPyramidMesh = 0U;
  g_deferredMutationCount = 0U;
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
  g_moduleLoadDepth = 0U;
  for (std::size_t i = 0U; i < kMaxFaultedEntities; ++i) {
    g_entityFaulted[i] = false;
  }
  for (std::size_t i = 0U; i < kMaxProfilerEntries; ++i) {
    g_profilerEntries[i] = ProfilerEntry{};
  }
  for (std::size_t i = 0U; i < kMaxBreakpoints; ++i) {
    g_breakpoints[i] = DebugBreakpoint{};
  }
  g_watchCount = 0U;
  g_lastWatchOutput[0] = '\0';
  g_lastCallstack[0] = '\0';
  g_lastBreakpointFile[0] = '\0';
  g_lastBreakpointLine = 0;
  g_breakpointHitCount = 0U;
  g_profilerEnabled = false;
  g_debuggerEnabled = false;
  g_godModeEnabled = false;
  g_noclipEnabled = false;
  for (std::size_t i = 0U; i < kMaxEntityPools; ++i) {
    g_entityPools[i] = runtime::EntityPool{};
  }
  g_entityPoolCount = 0U;
  std::snprintf(g_gameMode, sizeof(g_gameMode), "%s", "default");
  std::snprintf(g_gameState, sizeof(g_gameState), "%s", "startup");
  for (std::size_t i = 0U; i < kMaxPlayerControllers; ++i) {
    g_playerControllerEntities[i] = 0U;
  }
  g_playerControllers.reset();
  // Note: g_persistentGameState is NOT cleared here — it persists across
  // scene resets. The GameMode is reset via World's own reset path.
}

void bind_runtime_world(runtime::World *world) noexcept {
  g_world = world;
  // Also register/update in the global service locator.
  core::global_service_locator().register_service<runtime::World>(world);
}

void bind_runtime_services(const RuntimeServices *services) noexcept {
  g_services = services;
  core::global_service_locator().register_service<RuntimeServices>(
      const_cast<RuntimeServices *>(services));
}

void set_default_mesh_asset_id(std::uint32_t assetId) noexcept {
  g_defaultMeshAssetId = assetId;
}

void set_builtin_mesh_ids(std::uint32_t planeMesh, std::uint32_t cubeMesh,
                          std::uint32_t sphereMesh, std::uint32_t cylinderMesh,
                          std::uint32_t capsuleMesh,
                          std::uint32_t pyramidMesh) noexcept {
  g_builtinPlaneMesh = planeMesh;
  g_builtinCubeMesh = cubeMesh;
  g_builtinSphereMesh = sphereMesh;
  g_builtinCylinderMesh = cylinderMesh;
  g_builtinCapsuleMesh = capsuleMesh;
  g_builtinPyramidMesh = pyramidMesh;
}

void set_frame_time(float deltaSeconds, float totalSeconds) noexcept {
  g_deltaSeconds = deltaSeconds;
  g_totalSeconds = totalSeconds;
}

bool load_script(const char *path) noexcept {
  if (g_state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "scripting not initialized");
    return false;
  }

  if (path == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "script path is null");
    return false;
  }

  if (luaL_loadfile(g_state, path) != LUA_OK) {
    log_lua_error("load_script");
    return false;
  }

  if (lua_pcall(g_state, 0, 0, 0) != LUA_OK) {
    log_lua_error("load_script");
    return false;
  }

  return true;
}

bool call_script_function(const char *name) noexcept {
  if (g_state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "scripting not initialized");
    return false;
  }

  if (name == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "script function name is null");
    return false;
  }

  lua_getglobal(g_state, name);
  if (!lua_isfunction(g_state, -1)) {
    lua_pop(g_state, 1);
    return false;
  }

  if (lua_pcall(g_state, 0, 0, 0) != LUA_OK) {
    log_lua_error("call_script_function");
    return false;
  }

  return true;
}

bool call_script_function_float(const char *name, float arg) noexcept {
  if (g_state == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "scripting not initialized");
    return false;
  }

  if (name == nullptr) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "script function name is null");
    return false;
  }

  lua_getglobal(g_state, name);
  if (!lua_isfunction(g_state, -1)) {
    lua_pop(g_state, 1);
    return false;
  }

  lua_pushnumber(g_state, static_cast<lua_Number>(arg));
  if (lua_pcall(g_state, 1, 0, 0) != LUA_OK) {
    log_lua_error("call_script_function_float");
    return false;
  }

  return true;
}

void dispatch_physics_callbacks(const std::uint32_t *pairData,
                                std::size_t pairCount) noexcept {
  if ((g_state == nullptr) || (pairData == nullptr) || (pairCount == 0U)) {
    return;
  }

  for (std::size_t i = 0U; i < pairCount; ++i) {
    const lua_Integer eA = static_cast<lua_Integer>(pairData[i * 2U]);
    const lua_Integer eB = static_cast<lua_Integer>(pairData[i * 2U + 1U]);

    // Call all registered handlers.
    for (std::size_t h = 0U; h < kMaxCollisionHandlers; ++h) {
      if (g_collisionHandlers[h] == LUA_NOREF) {
        continue;
      }
      lua_rawgeti(g_state, LUA_REGISTRYINDEX, g_collisionHandlers[h]);
      if (!lua_isfunction(g_state, -1)) {
        lua_pop(g_state, 1);
        continue;
      }
      lua_pushinteger(g_state, eA);
      lua_pushinteger(g_state, eB);
      if (lua_pcall(g_state, 2, 0, 0) != LUA_OK) {
        log_lua_error("on_collision_handler");
      }
    }

    // Also call the legacy global on_collision for backward compatibility.
    lua_getglobal(g_state, "on_collision");
    if (lua_isfunction(g_state, -1)) {
      lua_pushinteger(g_state, eA);
      lua_pushinteger(g_state, eB);
      if (lua_pcall(g_state, 2, 0, 0) != LUA_OK) {
        log_lua_error("on_collision");
      }
    } else {
      lua_pop(g_state, 1);
    }
  }
}

namespace {
std::int64_t get_file_mtime(const char *path) noexcept {
  if ((path == nullptr) || (path[0] == '\0')) {
    return 0;
  }
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
    return 0;
  }
  ULARGE_INTEGER ul{};
  ul.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ul.HighPart = data.ftLastWriteTime.dwHighDateTime;
  return static_cast<std::int64_t>(ul.QuadPart);
#else
  struct stat st{};
  if (stat(path, &st) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(st.st_mtime);
#endif
}
} // anonymous namespace

void set_frame_index(std::uint32_t frameIndex) noexcept {
  g_frameIndex = frameIndex;
}

void flush_deferred_mutations() noexcept {
  if ((g_world == nullptr) || (g_services == nullptr) ||
      (g_deferredMutationCount == 0U) || !can_apply_mutations_now()) {
    return;
  }

  const std::size_t count = g_deferredMutationCount;
  g_deferredMutationCount = 0U;
  for (std::size_t i = 0U; i < count; ++i) {
    const DeferredMutation &mutation = g_deferredMutations[i];
    switch (mutation.type) {
    case DeferredMutationType::DestroyEntity:
      static_cast<void>(
          g_services->destroy_entity_op(g_world, mutation.entity.index));
      break;
    case DeferredMutationType::SetTransform: {
      const bool transformUpdated = g_services->add_transform_op(
          g_world, mutation.entity.index, mutation.transform);
      if (transformUpdated && mutation.setMovementAuthority) {
        static_cast<void>(g_services->set_movement_authority_op(
            g_world, mutation.entity.index, mutation.movementAuthority));
      }
      break;
    }
    case DeferredMutationType::AddRigidBody:
      static_cast<void>(g_services->add_rigid_body_op(
          g_world, mutation.entity.index, mutation.rigidBody));
      break;
    case DeferredMutationType::AddCollider:
      static_cast<void>(g_services->add_collider_op(
          g_world, mutation.entity.index, mutation.collider));
      break;
    case DeferredMutationType::AddMeshComponent:
      static_cast<void>(g_services->add_mesh_component_op(
          g_world, mutation.entity.index, mutation.meshComponent));
      break;
    case DeferredMutationType::AddNameComponent:
      static_cast<void>(g_services->add_name_component_op(
          g_world, mutation.entity.index, mutation.nameComponent));
      break;
    case DeferredMutationType::AddLightComponent:
      static_cast<void>(g_services->add_light_component_op(
          g_world, mutation.entity.index, mutation.lightComponent));
      break;
    case DeferredMutationType::RemoveLightComponent:
      static_cast<void>(g_services->remove_light_component_op(
          g_world, mutation.entity.index));
      break;
    case DeferredMutationType::AddScriptComponent:
      static_cast<void>(g_services->add_script_component_op(
          g_world, mutation.entity.index, mutation.scriptComponent));
      break;
    case DeferredMutationType::RemoveScriptComponent:
      static_cast<void>(g_services->remove_script_component_op(
          g_world, mutation.entity.index));
      break;
    }
  }
}

void tick_timers() noexcept {
  if (g_state == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < kMaxTimers; ++i) {
    if (!g_timers[i].active) {
      continue;
    }
    if (g_totalSeconds < g_timers[i].fireAt) {
      continue;
    }
    // Fire.
    lua_rawgeti(g_state, LUA_REGISTRYINDEX, g_timers[i].luaRef);
    if (lua_isfunction(g_state, -1)) {
      if (lua_pcall(g_state, 0, 0, 0) != LUA_OK) {
        log_lua_error("timer");
      }
    } else {
      lua_pop(g_state, 1);
    }
    if (g_timers[i].repeat) {
      g_timers[i].fireAt += g_timers[i].interval;
    } else {
      luaL_unref(g_state, LUA_REGISTRYINDEX, g_timers[i].luaRef);
      g_timers[i].luaRef = LUA_NOREF;
      g_timers[i].active = false;
    }
  }
}

void tick_coroutines() noexcept {
  if (g_state == nullptr) {
    return;
  }
  for (std::size_t i = 0U; i < kMaxCoroutines; ++i) {
    if (!g_coroutines[i].active) {
      continue;
    }
    if (g_totalSeconds < g_coroutines[i].wakeAt) {
      continue;
    }
    int nresults = 0;
    const int status =
        lua_resume(g_coroutines[i].thread, g_state, 0, &nresults);
    if (status == LUA_OK) {
      g_coroutines[i].active = false;
      g_coroutines[i].thread = nullptr;
    } else if (status == LUA_YIELD) {
      float wakeAt = g_totalSeconds; // default: next tick
      if (nresults >= 1) {
        if (lua_isnumber(g_coroutines[i].thread, -1)) {
          const float secs =
              static_cast<float>(lua_tonumber(g_coroutines[i].thread, -1));
          wakeAt = g_totalSeconds + secs;
        }
        lua_pop(g_coroutines[i].thread, nresults);
      }
      g_coroutines[i].wakeAt = wakeAt;
    } else {
      log_lua_error("coroutine");
      g_coroutines[i].active = false;
      g_coroutines[i].thread = nullptr;
    }
  }
}

void clear_coroutines() noexcept {
  for (std::size_t i = 0U; i < kMaxCoroutines; ++i) {
    g_coroutines[i] = CoroutineEntry{};
  }
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

void clear_pending_scene_op() noexcept {
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
}

void watch_script_file(const char *path) noexcept {
  if (path == nullptr) {
    return;
  }
  copy_c_string(g_watchedPath, sizeof(g_watchedPath), path);
  g_watchedMtime = get_file_mtime(path);
}

void check_script_reload() noexcept {
  if (g_watchedPath[0] == '\0') {
    return;
  }
  const std::int64_t mtime = get_file_mtime(g_watchedPath);
  if ((mtime != 0) && (mtime != g_watchedMtime)) {
    g_watchedMtime = mtime;
    core::log_message(core::LogLevel::Info, "scripting",
                      "hot-reloading script");
    load_script(g_watchedPath);
  }
}

void dispatch_entity_scripts_start() noexcept {
  if ((g_state == nullptr) || (g_world == nullptr)) {
    return;
  }

  g_world->for_each<runtime::ScriptComponent>(
      [](runtime::Entity entity, const runtime::ScriptComponent &sc) noexcept {
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
        call_module_function(ref, "on_begin_play", "on_start", entity.index,
                             false, 0.0F);
      });
}

void dispatch_entity_scripts_begin_play(runtime::World *world) noexcept {
  if ((g_state == nullptr) || (world == nullptr)) {
    return;
  }

  world->for_each_needs_begin_play([world](runtime::Entity entity) noexcept {
    world->mark_begin_play_done(entity);
    // Only dispatch if entity has a ScriptComponent.
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
    call_module_function(ref, "on_begin_play", "on_start", entity.index, false,
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
    // Fire EndPlay even for faulted entities (best effort cleanup).
    const int ref = get_or_load_entity_script_module(sc->scriptPath);
    if (ref == LUA_NOREF) {
      return;
    }
    static_cast<void>(call_module_function(ref, "on_end_play", "on_end",
                                           entity.index, false, 0.0F));
  });
}

void dispatch_entity_scripts_update(float dt) noexcept {
  if ((g_state == nullptr) || (g_world == nullptr)) {
    return;
  }

  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    if (!g_entityScriptModules[i].reloaded) {
      continue;
    }

    const char *reloadedPath = g_entityScriptModules[i].path;
    const int moduleRef = g_entityScriptModules[i].registryRef;
    g_world->for_each<runtime::ScriptComponent>(
        [reloadedPath, moduleRef](runtime::Entity entity,
                                  const runtime::ScriptComponent &sc) noexcept {
          if (std::strcmp(sc.scriptPath, reloadedPath) != 0) {
            return;
          }
          // Clear faulted state on reload.
          if (entity.index < kMaxFaultedEntities) {
            g_entityFaulted[entity.index] = false;
          }

          static_cast<void>(call_module_function(
              moduleRef, "on_reload", nullptr, entity.index, false, 0.0F));
          static_cast<void>(call_module_function(moduleRef, "on_begin_play",
                                                 "on_start", entity.index,
                                                 false, 0.0F));
        });
    g_entityScriptModules[i].reloaded = false;
  }

  g_world->for_each<runtime::ScriptComponent>(
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
        call_module_function(ref, "on_tick", "on_update", entity.index, true,
                             dt);
      });
}

void dispatch_entity_scripts_end() noexcept {
  if ((g_state == nullptr) || (g_world == nullptr)) {
    return;
  }

  g_world->for_each<runtime::ScriptComponent>(
      [](runtime::Entity entity, const runtime::ScriptComponent &sc) noexcept {
        if (sc.scriptPath[0] == '\0') {
          return;
        }
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
          return;
        }
        static_cast<void>(call_module_function(ref, "on_end_play", "on_end",
                                               entity.index, false, 0.0F));
      });
}

void clear_entity_script_modules() noexcept {
  if (g_state == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    if (g_entityScriptModules[i].registryRef != LUA_NOREF) {
      luaL_unref(g_state, LUA_REGISTRYINDEX,
                 g_entityScriptModules[i].registryRef);
      g_entityScriptModules[i].registryRef = LUA_NOREF;
    }
    g_entityScriptModules[i].path[0] = '\0';
    g_entityScriptModules[i].mtime = 0;
    g_entityScriptModules[i].reloaded = false;
  }
  g_entityScriptModuleCount = 0U;
}

} // namespace engine::scripting
