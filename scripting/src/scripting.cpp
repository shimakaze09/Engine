// Implements scripting behavior for the Engine Lua scripting system.

#include "engine/scripting/scripting.h"
#include "engine/scripting/bindable_api.h"
#include "engine/scripting/dap_server.h"
#include "collision_bindings.h"
#include "coroutine_bindings.h"
#include "debug_bindings.h"
#include "deferred_mutations.h"
#include "entity_script_bindings.h"
#include "game_bindings.h"
#include "persist_bindings.h"
#include "runtime_binding.h"
#include "timer_bindings.h"
#include "touch_bindings.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "engine/core/console.h"
#include "engine/core/input.h"
#include "engine/core/input_map.h"
#include "engine/core/logging.h"
#include "engine/math/quat.h"
#include "engine/runtime/entity_pool.h"
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

/// Handles register generated bindings.
void register_generated_bindings(lua_State *L) noexcept;
namespace {

lua_State *g_state = nullptr;
std::uint64_t g_defaultMeshAssetId = 0ULL;
std::uint64_t g_builtinPlaneMesh = 0ULL;
std::uint64_t g_builtinCubeMesh = 0ULL;
std::uint64_t g_builtinSphereMesh = 0ULL;
std::uint64_t g_builtinCylinderMesh = 0ULL;
std::uint64_t g_builtinCapsuleMesh = 0ULL;
std::uint64_t g_builtinPyramidMesh = 0ULL;
/// Handles k default gravity.
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

/// Handles copy c string.
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

/// Handles copy clone name.
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

/// Returns the requested value for file mtime.
std::int64_t get_file_mtime(const char *path) noexcept;

// Memory limit for the Lua allocator (bytes). Default 64MB.
constexpr std::size_t kDefaultMemoryLimit = 64U * 1024U * 1024U;
std::size_t g_memoryLimit = kDefaultMemoryLimit;
std::size_t g_memoryUsed = 0U;

// Custom allocator wrapper that enforces memory limit.
void *sandbox_alloc(void * /*ud*/, void *ptr, std::size_t osize,
                    std::size_t nsize) noexcept {
  if (nsize == 0U) {
    if (osize > 0U) {
      g_memoryUsed = (g_memoryUsed >= osize) ? (g_memoryUsed - osize) : 0U;
    }
    std::free(ptr);
    return nullptr;
  }
  if (nsize > osize && (g_memoryUsed + (nsize - osize)) > g_memoryLimit) {
    return nullptr; // Memory limit exceeded.
  }
  void *newPtr = std::realloc(ptr, nsize);
  if (newPtr != nullptr) {
    if (nsize > osize) {
      g_memoryUsed += (nsize - osize);
    } else {
      const std::size_t freed = osize - nsize;
      g_memoryUsed = (g_memoryUsed >= freed) ? (g_memoryUsed - freed) : 0U;
    }
  }
  return newPtr;
}

bool g_godModeEnabled = false;
bool g_noclipEnabled = false;

constexpr std::uint64_t kLuaEntityIndexMask = 0xFFFFFFFFULL;
constexpr unsigned kLuaEntityGenerationShift = 32U;

/// Handles refresh lua hook.
void refresh_lua_hook() noexcept;
/// Encodes a generated runtime entity into Lua's numeric handle format.
bool encode_lua_entity_handle(runtime::Entity entity,
                              lua_Integer *outHandle) noexcept {
  if ((outHandle == nullptr) || (entity.index == 0U) ||
      (entity.index > static_cast<std::uint32_t>(runtime::World::kMaxEntities)) ||
      (entity.generation == 0U)) {
    return false;
  }

  const std::uint64_t encodedGeneration =
      static_cast<std::uint64_t>(entity.generation - 1U);
  const std::uint64_t rawHandle =
      (encodedGeneration << kLuaEntityGenerationShift) |
      static_cast<std::uint64_t>(entity.index);
  if ((rawHandle == 0ULL) ||
      (rawHandle >
       static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max()))) {
    return false;
  }

  *outHandle = static_cast<lua_Integer>(rawHandle);
  return true;
}

/// Pushes a generated runtime entity as a Lua handle.
void push_entity_handle(lua_State *state, runtime::Entity entity) noexcept {
  lua_Integer handle = 0;
  if (!encode_lua_entity_handle(entity, &handle)) {
    lua_pushnil(state);
    return;
  }

  lua_pushinteger(state, handle);
}

/// Returns the current generated entity for an index, or invalid.
runtime::Entity entity_from_index(std::uint32_t entityIndex) noexcept {
  if (runtime_binding().world == nullptr) {
    return runtime::kInvalidEntity;
  }
  return runtime_binding().world->find_entity_by_index(entityIndex);
}

/// Pushes the current generated entity for an index as a Lua handle.
void push_entity_handle_from_index(lua_State *state,
                                   std::uint32_t entityIndex) noexcept {
  push_entity_handle(state, entity_from_index(entityIndex));
}

/// Decodes Lua's numeric entity handle format without requiring it to be live.
bool decode_entity_handle_value(std::uint64_t rawHandle,
                                runtime::Entity *outEntity) noexcept {
  if ((outEntity == nullptr) || (rawHandle == 0ULL)) {
    return false;
  }

  const std::uint32_t entityIndex =
      static_cast<std::uint32_t>(rawHandle & kLuaEntityIndexMask);
  const std::uint64_t encodedGeneration =
      rawHandle >> kLuaEntityGenerationShift;
  if ((entityIndex == 0U) ||
      (entityIndex > static_cast<std::uint32_t>(runtime::World::kMaxEntities)) ||
      (encodedGeneration >
       static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max() -
                                  1U))) {
    return false;
  }

  *outEntity = runtime::Entity{
      entityIndex, static_cast<std::uint32_t>(encodedGeneration + 1ULL)};
  return true;
}

/// Decodes Lua's numeric entity handle format without requiring it to be live.
bool decode_lua_entity_handle(lua_State *state, int index,
                              runtime::Entity *outEntity) noexcept {
  if ((outEntity == nullptr) || !lua_isnumber(state, index)) {
    return false;
  }

  const lua_Integer rawHandleSigned = lua_tointeger(state, index);
  if (rawHandleSigned <= 0) {
    return false;
  }

  return decode_entity_handle_value(static_cast<std::uint64_t>(rawHandleSigned),
                                    outEntity);
}

/// Reads vec3 args data.
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

/// Reads entity data.
bool read_entity(lua_State *state, int index,
                 runtime::Entity *outEntity) noexcept {
  if ((runtime_binding().world == nullptr) || (outEntity == nullptr)) {
    return false;
  }

  runtime::Entity decoded{};
  if (!decode_lua_entity_handle(state, index, &decoded) ||
      !runtime_binding().world->is_alive(decoded)) {
    return false;
  }

  *outEntity = decoded;
  return true;
}

/// Handles refresh lua hook.
void refresh_lua_hook() noexcept {
  refresh_debug_lua_hook();
}

/// Handles log lua error.
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

/// Handles lua engine log.
int lua_engine_log(lua_State *state) noexcept {
  const char *message = lua_tostring(state, 1);
  if (message == nullptr) {
    message = "";
  }

  core::log_message(core::LogLevel::Info, "script", message);
  return 0;
}

/// Handles lua engine get entity count.
int lua_engine_get_entity_count(lua_State *state) noexcept {
  const std::size_t count = (runtime_binding().world != nullptr && runtime_binding().services != nullptr)
                                ? runtime_binding().services->get_transform_count(runtime_binding().world)
                                : 0U;
  lua_pushinteger(state, static_cast<lua_Integer>(count));
  return 1;
}

/// Handles lua engine spawn entity.
int lua_engine_spawn_entity(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      !can_apply_mutations_now()) {
    lua_pushnil(state);
    return 1;
  }

  // Only valid during Input; create_entity returns 0 otherwise.
  const std::uint32_t entityIndex = runtime_binding().services->create_entity_op(runtime_binding().world);
  if (entityIndex == 0U) {
    lua_pushnil(state);
    return 1;
  }

  push_entity_handle_from_index(state, entityIndex);
  return 1;
}

/// Handles lua engine destroy entity.
int lua_engine_destroy_entity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  clear_player_controller_entity(entity);
  const bool ok = apply_or_queue_destroy_entity(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine is alive.
int lua_engine_is_alive(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity{};
  if (!decode_lua_entity_handle(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, runtime_binding().world->is_alive(entity) ? 1 : 0);
  return 1;
}

/// Handles lua engine get position.
int lua_engine_get_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  if (runtime_binding().services == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Transform *transform =
      runtime_binding().services->get_transform_read_ptr(runtime_binding().world, entity.index);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(transform->position.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->position.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->position.z));
  return 3;
}

/// Handles lua engine set position.
int lua_engine_set_position(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 position{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &position)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  if (runtime_binding().services != nullptr) {
    static_cast<void>(
        runtime_binding().services->get_transform_op(runtime_binding().world, entity.index, &transform));
  }
  transform.position = position;

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get velocity.
int lua_engine_get_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.velocity.z));
  return 3;
}

/// Handles lua engine add rigid body.
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

/// Handles lua engine set velocity.
int lua_engine_set_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 velocity{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &velocity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
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

/// Handles lua engine set acceleration.
int lua_engine_set_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 acceleration{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &acceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
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

/// Handles lua engine set additional acceleration.
int lua_engine_set_additional_acceleration(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 additionalAcceleration{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &additionalAcceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
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

/// Handles lua engine get angular velocity.
int lua_engine_get_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.x));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.y));
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.angularVelocity.z));
  return 3;
}

/// Handles lua engine set angular velocity.
int lua_engine_set_angular_velocity(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 angVel{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &angVel)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
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

/// Handles lua engine set mesh.
int lua_engine_set_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer meshId = lua_tointeger(state, 2);
  if (meshId <= 0) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  const runtime::MeshComponent *existing =
      runtime_binding().world->get_mesh_component_ptr(entity);
  if (existing != nullptr) {
    component = *existing;
  }

  component.meshAssetId = static_cast<std::uint64_t>(meshId);
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get default mesh asset id.
int lua_engine_get_default_mesh_asset_id(lua_State *state) noexcept {
  if (g_defaultMeshAssetId == 0ULL) {
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
  if ((runtime_binding().world == nullptr) || !can_apply_mutations_now() ||
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

  std::uint64_t meshId = g_defaultMeshAssetId;
  math::Vec3 halfExtents(0.5F, 0.5F, 0.5F);
  runtime::ColliderShape colliderShape = runtime::ColliderShape::AABB;

  if (std::strcmp(shape, "cube") == 0) {
    meshId =
        (g_builtinCubeMesh != 0ULL) ? g_builtinCubeMesh : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "sphere") == 0) {
    meshId = (g_builtinSphereMesh != 0ULL) ? g_builtinSphereMesh
                                           : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F); // halfExtents.x = radius
    colliderShape = runtime::ColliderShape::Sphere;
  } else if (std::strcmp(shape, "cylinder") == 0) {
    meshId = (g_builtinCylinderMesh != 0ULL) ? g_builtinCylinderMesh
                                             : g_defaultMeshAssetId;
    // Best available approximation for a round cylinder: upright capsule.
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::Capsule;
  } else if (std::strcmp(shape, "capsule") == 0) {
    meshId = (g_builtinCapsuleMesh != 0ULL) ? g_builtinCapsuleMesh
                                            : g_defaultMeshAssetId;
    // Capsule: halfExtents.x = radius, halfExtents.y = halfHeight.
    halfExtents = math::Vec3(0.5F, 0.5F, 0.5F);
    colliderShape = runtime::ColliderShape::Capsule;
  } else if (std::strcmp(shape, "pyramid") == 0) {
    meshId = (g_builtinPyramidMesh != 0ULL) ? g_builtinPyramidMesh
                                            : g_defaultMeshAssetId;
    halfExtents = math::Vec3(0.5F, 0.5F, 0.58F);
    colliderShape = runtime::ColliderShape::AABB;
  } else if (std::strcmp(shape, "plane") == 0) {
    meshId = (g_builtinPlaneMesh != 0ULL) ? g_builtinPlaneMesh
                                          : g_defaultMeshAssetId;
    halfExtents = math::Vec3(5.0F, 0.1F, 5.0F);
    colliderShape = runtime::ColliderShape::AABB;
  }

  const runtime::Entity entity = runtime_binding().world->create_entity();
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  runtime::Transform transform{};
  transform.position = pos;
  static_cast<void>(runtime_binding().world->add_transform(entity, transform));

  runtime::RigidBody rigidBody{};
  rigidBody.inverseMass = 1.0F;
  static_cast<void>(runtime_binding().world->add_rigid_body(entity, rigidBody));

  runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  collider.shape = colliderShape;
  static_cast<void>(runtime_binding().world->add_collider(entity, collider));

  if (meshId != 0ULL) {
    runtime::MeshComponent meshComp{};
    meshComp.meshAssetId = meshId;
    meshComp.albedo = albedo;
    static_cast<void>(runtime_binding().world->add_mesh_component(entity, meshComp));
  }

  push_entity_handle(state, entity);
  return 1;
}

/// Handles lua engine set albedo.
int lua_engine_set_albedo(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 albedo{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &albedo)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::MeshComponent component{};
  const runtime::MeshComponent *existing =
      runtime_binding().world->get_mesh_component_ptr(entity);
  if (existing != nullptr) {
    component = *existing;
  }

  component.albedo = albedo;
  const bool ok = apply_or_queue_mesh_component(entity, component);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine set name.
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

/// Handles lua engine get name.
int lua_engine_get_name(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }

  runtime::NameComponent component{};
  if ((runtime_binding().services == nullptr) ||
      !runtime_binding().services->get_name_component_op(runtime_binding().world, entity.index, &component)) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushstring(state, component.name);
  return 1;
}

// engine.add_capsule_collider(entity, half_height, radius) → bool
int lua_engine_add_capsule_collider(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2) || !lua_isnumber(state, 3)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float halfHeight = static_cast<float>(lua_tonumber(state, 2));
  const float radius = static_cast<float>(lua_tonumber(state, 3));

  runtime::Collider collider{};
  collider.shape = runtime::ColliderShape::Capsule;
  collider.halfExtents = math::Vec3(radius, halfHeight, radius);

  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine add collider.
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

/// Handles lua engine set restitution.
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
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  collider.restitution = value;
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine set friction.
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
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  collider.staticFriction = staticF;
  collider.dynamicFriction = dynamicF;
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.create_physics_material(static_friction, dynamic_friction,
//                                restitution, density) → table
int lua_engine_create_physics_material(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3)) {
    lua_pushnil(state);
    return 1;
  }
  lua_createtable(state, 0, 4);
  lua_pushnumber(state, lua_tonumber(state, 1));
  lua_setfield(state, -2, "static_friction");
  lua_pushnumber(state, lua_tonumber(state, 2));
  lua_setfield(state, -2, "dynamic_friction");
  lua_pushnumber(state, lua_tonumber(state, 3));
  lua_setfield(state, -2, "restitution");
  const float density = lua_isnumber(state, 4)
                            ? static_cast<float>(lua_tonumber(state, 4))
                            : 1.0F;
  lua_pushnumber(state, static_cast<lua_Number>(density));
  lua_setfield(state, -2, "density");
  return 1;
}

// engine.set_collider_material(entity, material_table) → bool
int lua_engine_set_collider_material(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_istable(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_getfield(state, 2, "static_friction");
  if (lua_isnumber(state, -1)) {
    collider.staticFriction = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  lua_getfield(state, 2, "dynamic_friction");
  if (lua_isnumber(state, -1)) {
    collider.dynamicFriction = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  lua_getfield(state, 2, "restitution");
  if (lua_isnumber(state, -1)) {
    collider.restitution = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  lua_getfield(state, 2, "density");
  if (lua_isnumber(state, -1)) {
    collider.density = static_cast<float>(lua_tonumber(state, -1));
  }
  lua_pop(state, 1);

  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_collision_layer(entity, layer_bits) → bool
int lua_engine_set_collision_layer(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.collisionLayer = static_cast<std::uint32_t>(lua_tointeger(state, 2));
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// engine.set_collision_mask(entity, mask_bits) → bool
int lua_engine_set_collision_mask(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.collisionMask = static_cast<std::uint32_t>(lua_tointeger(state, 2));
  const bool ok = apply_or_queue_collider(entity, collider);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine delta time.
int lua_engine_delta_time(lua_State *state) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(g_deltaSeconds));
  return 1;
}

/// Handles lua engine elapsed time.
int lua_engine_elapsed_time(lua_State *state) noexcept {
  lua_pushnumber(state, static_cast<lua_Number>(g_totalSeconds));
  return 1;
}

/// Handles lua engine is key down.
int lua_engine_is_key_down(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int scancode = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_key_down(scancode) ? 1 : 0);
  return 1;
}

/// Handles lua engine is key pressed.
int lua_engine_is_key_pressed(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int scancode = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_key_pressed(scancode) ? 1 : 0);
  return 1;
}

/// Handles lua engine register action.
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

/// Handles lua engine register axis.
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

/// Handles lua engine is action down.
int lua_engine_is_action_down(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_down(name) ? 1 : 0);
  return 1;
}

/// Handles lua engine is action pressed.
int lua_engine_is_action_pressed(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushboolean(state, core::is_action_pressed(name) ? 1 : 0);
  return 1;
}

/// Handles lua engine get action value.
int lua_engine_get_action_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::action_value(name)));
  return 1;
}

/// Handles lua engine get axis value.
int lua_engine_get_axis_value(lua_State *state) noexcept {
  const char *name = lua_tostring(state, 1);
  lua_pushnumber(state, static_cast<lua_Number>(core::axis_value(name)));
  return 1;
}

/// Handles lua engine is gamepad connected.
int lua_engine_is_gamepad_connected(lua_State *state) noexcept {
  static_cast<void>(state);
  lua_pushboolean(state, core::is_gamepad_connected() ? 1 : 0);
  return 1;
}

/// Handles lua engine is gamepad button down.
int lua_engine_is_gamepad_button_down(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const int button = static_cast<int>(lua_tointeger(state, 1));
  lua_pushboolean(state, core::is_gamepad_button_down(button) ? 1 : 0);
  return 1;
}

/// Handles lua engine gamepad axis value.
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

/// Handles lua engine set player controller.
int lua_engine_set_player_controller(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer player = lua_tointeger(state, 1);
  const lua_Integer entityHandle = lua_tointeger(state, 2);
  const auto maxPlayerIndex =
      static_cast<lua_Integer>(std::numeric_limits<std::uint8_t>::max());
  if ((player < 0) || (player > maxPlayerIndex) ||
      (entityHandle < 0)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Entity entity = runtime::kInvalidEntity;
  if (entityHandle != 0) {
    if (!read_entity(state, 2, &entity)) {
      lua_pushboolean(state, 0);
      return 1;
    }
  }

  lua_pushboolean(state,
                  set_player_controller_entity(
                      static_cast<std::uint8_t>(player), entity)
                      ? 1
                      : 0);
  return 1;
}

/// Handles lua engine is god mode.
int lua_engine_is_god_mode(lua_State *state) noexcept {
  lua_pushboolean(state, g_godModeEnabled ? 1 : 0);
  return 1;
}

/// Handles lua engine is noclip.
int lua_engine_is_noclip(lua_State *state) noexcept {
  lua_pushboolean(state, g_noclipEnabled ? 1 : 0);
  return 1;
}

/// Handles lua engine get player controller.
int lua_engine_get_player_controller(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    lua_pushnil(state);
    return 1;
  }

  const lua_Integer player = lua_tointeger(state, 1);
  const auto maxPlayerIndex =
      static_cast<lua_Integer>(std::numeric_limits<std::uint8_t>::max());
  if ((player < 0) || (player > maxPlayerIndex)) {
    lua_pushnil(state);
    return 1;
  }

  const auto idx = static_cast<std::uint8_t>(player);
  const runtime::Entity entity = get_player_controller_entity(idx);
  if ((entity == runtime::kInvalidEntity) ||
      (runtime_binding().world == nullptr) ||
      !runtime_binding().world->is_alive(entity)) {
    lua_pushinteger(state, 0);
    return 1;
  }

  push_entity_handle(state, entity);
  return 1;
}

/// Handles lua engine set camera position.
int lua_engine_set_camera_position(lua_State *state) noexcept {
  math::Vec3 pos{};
  if (!read_vec3_args(state, 1, &pos) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_position == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_position(pos.x, pos.y, pos.z);
  return 0;
}

/// Handles lua engine set camera target.
int lua_engine_set_camera_target(lua_State *state) noexcept {
  math::Vec3 target{};
  if (!read_vec3_args(state, 1, &target) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_target == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_target(target.x, target.y, target.z);
  return 0;
}

/// Handles lua engine set camera up.
int lua_engine_set_camera_up(lua_State *state) noexcept {
  math::Vec3 up{};
  if (!read_vec3_args(state, 1, &up) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_up == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_up(up.x, up.y, up.z);
  return 0;
}

/// Handles lua engine set camera fov.
int lua_engine_set_camera_fov(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->set_camera_fov == nullptr)) {
    return 0;
  }
  runtime_binding().services->set_camera_fov(static_cast<float>(lua_tonumber(state, 1)));
  return 0;
}

// -- Camera Manager Lua bindings ------------------------------------------

// Engine.push_camera(entityIndex, posX,posY,posZ, tgtX,tgtY,tgtZ, priority
// [, blendSpeed])
int lua_engine_push_camera(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->push_camera_op == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float posX = static_cast<float>(luaL_checknumber(state, 2));
  const float posY = static_cast<float>(luaL_checknumber(state, 3));
  const float posZ = static_cast<float>(luaL_checknumber(state, 4));
  const float tgtX = static_cast<float>(luaL_checknumber(state, 5));
  const float tgtY = static_cast<float>(luaL_checknumber(state, 6));
  const float tgtZ = static_cast<float>(luaL_checknumber(state, 7));
  const float priority = static_cast<float>(luaL_checknumber(state, 8));
  float blendSpeed = 5.0F;
  if (lua_isnumber(state, 9)) {
    blendSpeed = static_cast<float>(lua_tonumber(state, 9));
  }
  const bool ok =
      runtime_binding().services->push_camera_op(runtime_binding().world, entity.index, posX, posY, posZ, tgtX,
                                 tgtY, tgtZ, priority, blendSpeed);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.pop_camera(entityIndex)
int lua_engine_pop_camera(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->pop_camera_op == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = runtime_binding().services->pop_camera_op(runtime_binding().world, entity.index);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_active_camera() -> posX,posY,posZ, tgtX,tgtY,tgtZ, fov | nil
int lua_engine_get_active_camera(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->get_active_camera_op == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  float posX = 0.0F;
  float posY = 0.0F;
  float posZ = 0.0F;
  float tgtX = 0.0F;
  float tgtY = 0.0F;
  float tgtZ = 0.0F;
  float fov = 0.0F;
  if (!runtime_binding().services->get_active_camera_op(runtime_binding().world, &posX, &posY, &posZ, &tgtX,
                                        &tgtY, &tgtZ, &fov)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<double>(posX));
  lua_pushnumber(state, static_cast<double>(posY));
  lua_pushnumber(state, static_cast<double>(posZ));
  lua_pushnumber(state, static_cast<double>(tgtX));
  lua_pushnumber(state, static_cast<double>(tgtY));
  lua_pushnumber(state, static_cast<double>(tgtZ));
  lua_pushnumber(state, static_cast<double>(fov));
  return 7;
}

// Engine.camera_shake(amplitude, frequency, duration [, decay])
int lua_engine_camera_shake(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->camera_shake_op == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const float amplitude = static_cast<float>(luaL_checknumber(state, 1));
  const float frequency = static_cast<float>(luaL_checknumber(state, 2));
  const float duration = static_cast<float>(luaL_checknumber(state, 3));
  float decay = 2.0F;
  if (lua_isnumber(state, 4)) {
    decay = static_cast<float>(lua_tonumber(state, 4));
  }
  const bool ok = runtime_binding().services->camera_shake_op(runtime_binding().world, amplitude, frequency,
                                              duration, decay);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// -- Spring Arm Lua bindings -----------------------------------------------

// Engine.add_spring_arm(entityIndex, armLength, offsetX, offsetY, offsetZ
// [, lagSpeed] [, collisionEnabled])
int lua_engine_add_spring_arm(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::SpringArmComponent arm{};
  arm.armLength = static_cast<float>(luaL_checknumber(state, 2));
  arm.currentLength = arm.armLength;
  arm.offset.x = static_cast<float>(luaL_checknumber(state, 3));
  arm.offset.y = static_cast<float>(luaL_checknumber(state, 4));
  arm.offset.z = static_cast<float>(luaL_checknumber(state, 5));
  if (lua_isnumber(state, 6)) {
    arm.lagSpeed = static_cast<float>(lua_tonumber(state, 6));
  }
  if (lua_isboolean(state, 7)) {
    arm.collisionEnabled = (lua_toboolean(state, 7) != 0);
  }
  const bool ok = runtime_binding().world->add_spring_arm(entity, arm);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Engine.get_spring_arm(entityIndex) -> armLength, currentLength, offX, offY,
// offZ, lagSpeed | nil
int lua_engine_get_spring_arm(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::SpringArmComponent arm{};
  if (!runtime_binding().world->get_spring_arm(entity, &arm)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<double>(arm.armLength));
  lua_pushnumber(state, static_cast<double>(arm.currentLength));
  lua_pushnumber(state, static_cast<double>(arm.offset.x));
  lua_pushnumber(state, static_cast<double>(arm.offset.y));
  lua_pushnumber(state, static_cast<double>(arm.offset.z));
  lua_pushnumber(state, static_cast<double>(arm.lagSpeed));
  return 6;
}

/// Handles lua engine set gravity.
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
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_gravity != nullptr)) {
    runtime_binding().services->set_gravity(runtime_binding().world, x, y, z);
  }
  return 0;
}

/// Handles lua engine get gravity.
int lua_engine_get_gravity(lua_State *state) noexcept {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->get_gravity == nullptr) ||
      !runtime_binding().services->get_gravity(runtime_binding().world, &x, &y, &z)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(x));
  lua_pushnumber(state, static_cast<lua_Number>(y));
  lua_pushnumber(state, static_cast<lua_Number>(z));
  return 3;
}

/// Handles lua engine raycast.
int lua_engine_raycast(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
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
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->raycast == nullptr) ||
      !runtime_binding().services->raycast(runtime_binding().world, ox, oy, oz, dx, dy, dz, maxDist, &hit)) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, hit.entityIndex);
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

// engine.raycast_all(ox,oy,oz, dx,dy,dz, max_dist [, mask]) → table of hits
int lua_engine_raycast_all(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->raycast_all == nullptr)) {
    lua_newtable(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7)) {
    lua_newtable(state);
    return 1;
  }
  const float ox = static_cast<float>(lua_tonumber(state, 1));
  const float oy = static_cast<float>(lua_tonumber(state, 2));
  const float oz = static_cast<float>(lua_tonumber(state, 3));
  const float dx = static_cast<float>(lua_tonumber(state, 4));
  const float dy = static_cast<float>(lua_tonumber(state, 5));
  const float dz = static_cast<float>(lua_tonumber(state, 6));
  const float maxDist = static_cast<float>(lua_tonumber(state, 7));
  const std::uint32_t mask =
      lua_isnumber(state, 8)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 8))
          : 0xFFFFFFFFU;

  constexpr std::size_t kMaxHits = 32U;
  RuntimeRaycastHit hits[kMaxHits]{};
  const std::size_t count = runtime_binding().services->raycast_all(
      runtime_binding().world, ox, oy, oz, dx, dy, dz, maxDist, hits, kMaxHits, mask);

  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    lua_createtable(state, 0, 8);
    push_entity_handle_from_index(state, hits[i].entityIndex);
    lua_setfield(state, -2, "entity");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].distance));
    lua_setfield(state, -2, "distance");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].pointX));
    lua_setfield(state, -2, "px");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].pointY));
    lua_setfield(state, -2, "py");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].pointZ));
    lua_setfield(state, -2, "pz");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].normalX));
    lua_setfield(state, -2, "nx");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].normalY));
    lua_setfield(state, -2, "ny");
    lua_pushnumber(state, static_cast<lua_Number>(hits[i].normalZ));
    lua_setfield(state, -2, "nz");
    lua_rawseti(state, -2, static_cast<int>(i + 1U));
  }
  return 1;
}

// engine.overlap_sphere(cx,cy,cz, radius [, mask]) → table of entity indices
int lua_engine_overlap_sphere(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->overlap_sphere == nullptr)) {
    lua_newtable(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4)) {
    lua_newtable(state);
    return 1;
  }
  const float cx = static_cast<float>(lua_tonumber(state, 1));
  const float cy = static_cast<float>(lua_tonumber(state, 2));
  const float cz = static_cast<float>(lua_tonumber(state, 3));
  const float radius = static_cast<float>(lua_tonumber(state, 4));
  const std::uint32_t mask =
      lua_isnumber(state, 5)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 5))
          : 0xFFFFFFFFU;

  constexpr std::size_t kMaxResults = 64U;
  std::uint32_t indices[kMaxResults]{};
  const std::size_t count = runtime_binding().services->overlap_sphere(
      runtime_binding().world, cx, cy, cz, radius, indices, kMaxResults, mask);

  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    push_entity_handle_from_index(state, indices[i]);
    lua_rawseti(state, -2, static_cast<int>(i + 1U));
  }
  return 1;
}

// engine.overlap_box(cx,cy,cz, hx,hy,hz [, mask]) → table of entity indices
int lua_engine_overlap_box(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->overlap_box == nullptr)) {
    lua_newtable(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6)) {
    lua_newtable(state);
    return 1;
  }
  const float cx = static_cast<float>(lua_tonumber(state, 1));
  const float cy = static_cast<float>(lua_tonumber(state, 2));
  const float cz = static_cast<float>(lua_tonumber(state, 3));
  const float hx = static_cast<float>(lua_tonumber(state, 4));
  const float hy = static_cast<float>(lua_tonumber(state, 5));
  const float hz = static_cast<float>(lua_tonumber(state, 6));
  const std::uint32_t mask =
      lua_isnumber(state, 7)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 7))
          : 0xFFFFFFFFU;

  constexpr std::size_t kMaxResults = 64U;
  std::uint32_t indices[kMaxResults]{};
  const std::size_t count = runtime_binding().services->overlap_box(
      runtime_binding().world, cx, cy, cz, hx, hy, hz, indices, kMaxResults, mask);

  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    push_entity_handle_from_index(state, indices[i]);
    lua_rawseti(state, -2, static_cast<int>(i + 1U));
  }
  return 1;
}

// engine.sweep_sphere(ox,oy,oz, radius, dx,dy,dz, max_dist [, mask])
int lua_engine_sweep_sphere(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->sweep_sphere == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7) || !lua_isnumber(state, 8)) {
    lua_pushnil(state);
    return 1;
  }
  const float ox = static_cast<float>(lua_tonumber(state, 1));
  const float oy = static_cast<float>(lua_tonumber(state, 2));
  const float oz = static_cast<float>(lua_tonumber(state, 3));
  const float radius = static_cast<float>(lua_tonumber(state, 4));
  const float dx = static_cast<float>(lua_tonumber(state, 5));
  const float dy = static_cast<float>(lua_tonumber(state, 6));
  const float dz = static_cast<float>(lua_tonumber(state, 7));
  const float maxDist = static_cast<float>(lua_tonumber(state, 8));
  const std::uint32_t mask =
      lua_isnumber(state, 9)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 9))
          : 0xFFFFFFFFU;

  RuntimeRaycastHit hit{};
  if (!runtime_binding().services->sweep_sphere(runtime_binding().world, ox, oy, oz, radius, dx, dy, dz,
                                maxDist, &hit, mask)) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, hit.entityIndex);
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

// engine.sweep_box(cx,cy,cz, hx,hy,hz, dx,dy,dz, max_dist [, mask])
int lua_engine_sweep_box(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->sweep_box == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2) ||
      !lua_isnumber(state, 3) || !lua_isnumber(state, 4) ||
      !lua_isnumber(state, 5) || !lua_isnumber(state, 6) ||
      !lua_isnumber(state, 7) || !lua_isnumber(state, 8) ||
      !lua_isnumber(state, 9) || !lua_isnumber(state, 10)) {
    lua_pushnil(state);
    return 1;
  }
  const float cx = static_cast<float>(lua_tonumber(state, 1));
  const float cy = static_cast<float>(lua_tonumber(state, 2));
  const float cz = static_cast<float>(lua_tonumber(state, 3));
  const float hx = static_cast<float>(lua_tonumber(state, 4));
  const float hy = static_cast<float>(lua_tonumber(state, 5));
  const float hz = static_cast<float>(lua_tonumber(state, 6));
  const float dx = static_cast<float>(lua_tonumber(state, 7));
  const float dy = static_cast<float>(lua_tonumber(state, 8));
  const float dz = static_cast<float>(lua_tonumber(state, 9));
  const float maxDist = static_cast<float>(lua_tonumber(state, 10));
  const std::uint32_t mask =
      lua_isnumber(state, 11)
          ? static_cast<std::uint32_t>(lua_tointeger(state, 11))
          : 0xFFFFFFFFU;

  RuntimeRaycastHit hit{};
  if (!runtime_binding().services->sweep_box(runtime_binding().world, cx, cy, cz, hx, hy, hz, dx, dy, dz,
                             maxDist, &hit, mask)) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, hit.entityIndex);
  lua_pushnumber(state, static_cast<lua_Number>(hit.distance));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.pointZ));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalX));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalY));
  lua_pushnumber(state, static_cast<lua_Number>(hit.normalZ));
  return 8;
}

/// Handles lua engine add distance joint.
int lua_engine_add_distance_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_distance_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const float dist = lua_isnumber(state, 3)
                         ? static_cast<float>(lua_tonumber(state, 3))
                         : 1.0F;
  const std::uint32_t id = runtime_binding().services->add_distance_joint(
      runtime_binding().world, entityA.index, entityB.index, dist);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

/// Handles lua engine remove joint.
int lua_engine_remove_joint(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->remove_joint != nullptr)) {
    runtime_binding().services->remove_joint(
        runtime_binding().world, static_cast<std::uint32_t>(lua_tointeger(state, 1)));
  }
  return 0;
}

// engine.add_hinge_joint(entityA, entityB, pivotX, pivotY, pivotZ, axisX,
// axisY, axisZ)
int lua_engine_add_hinge_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_hinge_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const auto px = static_cast<float>(luaL_optnumber(state, 3, 0.0));
  const auto py = static_cast<float>(luaL_optnumber(state, 4, 0.0));
  const auto pz = static_cast<float>(luaL_optnumber(state, 5, 0.0));
  const auto ax = static_cast<float>(luaL_optnumber(state, 6, 0.0));
  const auto ay = static_cast<float>(luaL_optnumber(state, 7, 1.0));
  const auto az = static_cast<float>(luaL_optnumber(state, 8, 0.0));
  const std::uint32_t id = runtime_binding().services->add_hinge_joint(
      runtime_binding().world, entityA.index, entityB.index, px, py, pz, ax, ay, az);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

// engine.add_ball_socket_joint(entityA, entityB, pivotX, pivotY, pivotZ)
int lua_engine_add_ball_socket_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) ||
      (runtime_binding().services->add_ball_socket_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const auto px = static_cast<float>(luaL_optnumber(state, 3, 0.0));
  const auto py = static_cast<float>(luaL_optnumber(state, 4, 0.0));
  const auto pz = static_cast<float>(luaL_optnumber(state, 5, 0.0));
  const std::uint32_t id = runtime_binding().services->add_ball_socket_joint(
      runtime_binding().world, entityA.index, entityB.index, px, py, pz);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

// engine.add_slider_joint(entityA, entityB, axisX, axisY, axisZ)
int lua_engine_add_slider_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_slider_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const auto ax = static_cast<float>(luaL_optnumber(state, 3, 1.0));
  const auto ay = static_cast<float>(luaL_optnumber(state, 4, 0.0));
  const auto az = static_cast<float>(luaL_optnumber(state, 5, 0.0));
  const std::uint32_t id = runtime_binding().services->add_slider_joint(
      runtime_binding().world, entityA.index, entityB.index, ax, ay, az);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

// engine.add_spring_joint(entityA, entityB, restLength, stiffness, damping)
int lua_engine_add_spring_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_spring_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const auto rest = static_cast<float>(luaL_optnumber(state, 3, 1.0));
  const auto stiff = static_cast<float>(luaL_optnumber(state, 4, 100.0));
  const auto damp = static_cast<float>(luaL_optnumber(state, 5, 1.0));
  const std::uint32_t id = runtime_binding().services->add_spring_joint(
      runtime_binding().world, entityA.index, entityB.index, rest, stiff, damp);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

// engine.add_fixed_joint(entityA, entityB)
int lua_engine_add_fixed_joint(lua_State *state) noexcept {
  runtime::Entity entityA{};
  runtime::Entity entityB{};
  if (!read_entity(state, 1, &entityA) || !read_entity(state, 2, &entityB) ||
      (runtime_binding().services == nullptr) || (runtime_binding().services->add_fixed_joint == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  const std::uint32_t id =
      runtime_binding().services->add_fixed_joint(runtime_binding().world, entityA.index, entityB.index);
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

// engine.set_joint_limits(jointId, minLimit, maxLimit)
int lua_engine_set_joint_limits(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_joint_limits != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    const auto minL = static_cast<float>(luaL_optnumber(state, 2, 0.0));
    const auto maxL = static_cast<float>(luaL_optnumber(state, 3, 0.0));
    runtime_binding().services->set_joint_limits(runtime_binding().world, id, minL, maxL);
  }
  return 0;
}

/// Handles lua engine wake body.
int lua_engine_wake_body(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    return 0;
  }
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->wake_body != nullptr)) {
    const auto idx = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->wake_body(runtime_binding().world, idx);
  }
  return 0;
}

/// Handles lua engine is sleeping.
int lua_engine_is_sleeping(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isnumber(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->is_sleeping == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto idx = static_cast<std::uint32_t>(lua_tointeger(state, 1));
  lua_pushboolean(state, runtime_binding().services->is_sleeping(runtime_binding().world, idx) ? 1 : 0);
  return 1;
}

/// Handles lua engine frame count.
int lua_engine_frame_count(lua_State *state) noexcept {
  lua_pushinteger(state, static_cast<lua_Integer>(g_frameIndex));
  return 1;
}

/// Handles lua engine load sound.
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
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->load_sound == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  lua_pushinteger(state,
                  static_cast<lua_Integer>(runtime_binding().services->load_sound(path)));
  return 1;
}

/// Handles lua engine unload sound.
int lua_engine_unload_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->unload_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->unload_sound(id);
  }
  return 0;
}

/// Handles lua engine play sound.
int lua_engine_play_sound(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->play_sound == nullptr)) {
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
  const bool ok = runtime_binding().services->play_sound(id, volume, pitch, loop);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine stop sound.
int lua_engine_stop_sound(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, 1));
    runtime_binding().services->stop_sound(id);
  }
  return 0;
}

/// Handles lua engine stop all sounds.
int lua_engine_stop_all_sounds(lua_State *state) noexcept {
  static_cast<void>(state);
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_all_sounds != nullptr)) {
    runtime_binding().services->stop_all_sounds();
  }
  return 0;
}

/// Handles lua engine set master volume.
int lua_engine_set_master_volume(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_master_volume != nullptr)) {
    const auto vol = static_cast<float>(lua_tonumber(state, 1));
    runtime_binding().services->set_master_volume(vol);
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
  const runtime::Transform *transform = runtime_binding().world->get_transform_read_ptr(entity);
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

/// Handles lua engine set rotation.
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
  static_cast<void>(runtime_binding().world->get_transform(entity, &transform));
  transform.rotation = math::Quat(qx, qy, qz, qw);

  const bool ok = apply_or_queue_transform(entity, transform, true,
                                           runtime::MovementAuthority::Script);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get scale.
int lua_engine_get_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::Transform *transform = runtime_binding().world->get_transform_read_ptr(entity);
  if (transform == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.x));
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.y));
  lua_pushnumber(state, static_cast<lua_Number>(transform->scale.z));
  return 3;
}

/// Handles lua engine set scale.
int lua_engine_set_scale(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 scale{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &scale)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Transform transform{};
  static_cast<void>(runtime_binding().world->get_transform(entity, &transform));
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
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(rigidBody.inverseMass));
  return 1;
}

/// Handles lua engine set inverse mass.
int lua_engine_set_inverse_mass(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::RigidBody rigidBody{};
  if (!runtime_binding().world->get_rigid_body(entity, &rigidBody)) {
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
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.x));
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.y));
  lua_pushnumber(state, static_cast<lua_Number>(collider.halfExtents.z));
  return 3;
}

/// Handles lua engine set half extents.
int lua_engine_set_half_extents(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 halfExtents{};
  if (!read_entity(state, 1, &entity) ||
      !read_vec3_args(state, 2, &halfExtents)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Collider collider{};
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  collider.halfExtents = halfExtents;
  lua_pushboolean(state, apply_or_queue_collider(entity, collider) ? 1 : 0);
  return 1;
}

/// Handles lua engine get restitution.
int lua_engine_get_restitution(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!runtime_binding().world->get_collider(entity, &collider)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(collider.restitution));
  return 1;
}

/// Handles lua engine get friction.
int lua_engine_get_friction(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Collider collider{};
  if (!runtime_binding().world->get_collider(entity, &collider)) {
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
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.x));
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.y));
  lua_pushnumber(state, static_cast<lua_Number>(mesh->albedo.z));
  return 3;
}

/// Handles lua engine get mesh.
int lua_engine_get_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(mesh->meshAssetId));
  return 1;
}

/// Handles lua engine set roughness.
int lua_engine_set_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!runtime_binding().world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.roughness = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

/// Handles lua engine get roughness.
int lua_engine_get_roughness(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->roughness));
  return 1;
}

/// Handles lua engine set metallic.
int lua_engine_set_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!runtime_binding().world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.metallic = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

/// Handles lua engine get metallic.
int lua_engine_get_metallic(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(mesh->metallic));
  return 1;
}

/// Handles lua engine set opacity.
int lua_engine_set_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::MeshComponent mesh{};
  if (!runtime_binding().world->get_mesh_component(entity, &mesh)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh.opacity = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, apply_or_queue_mesh_component(entity, mesh) ? 1 : 0);
  return 1;
}

/// Handles lua engine get opacity.
int lua_engine_get_opacity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  const runtime::MeshComponent *mesh = runtime_binding().world->get_mesh_component_ptr(entity);
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

/// Handles lua engine remove light.
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

/// Handles lua engine has light.
int lua_engine_has_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  lua_pushboolean(state, runtime_binding().world->has_light_component(entity) ? 1 : 0);
  return 1;
}

/// Handles lua engine set light color.
int lua_engine_set_light_color(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 color{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &color)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.color = color;
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get light color.
int lua_engine_get_light_color(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(light.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(light.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(light.color.z));
  return 3;
}

/// Handles lua engine set light intensity.
int lua_engine_set_light_intensity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.intensity = static_cast<float>(lua_tonumber(state, 2));
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine get light intensity.
int lua_engine_get_light_intensity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(light.intensity));
  return 1;
}

/// Handles lua engine set light direction.
int lua_engine_set_light_direction(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 dir{};
  if (!read_entity(state, 1, &entity) || !read_vec3_args(state, 2, &dir)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::LightComponent light{};
  if (!runtime_binding().world->get_light_component(entity, &light)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  light.direction = dir;
  const bool ok = apply_or_queue_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- PointLightComponent bindings ---

// Lua: engine.add_point_light(entity, r, g, b, intensity, radius) → boolean
static int lua_engine_add_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 6) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::PointLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 5));
  comp.radius = static_cast<float>(luaL_checknumber(state, 6));
  const bool ok = apply_or_queue_point_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.get_point_light(entity) → r, g, b, intensity, radius or nil
static int lua_engine_get_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::PointLightComponent comp{};
  if (!runtime_binding().world->get_point_light_component(entity, &comp)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.z));
  lua_pushnumber(state, static_cast<lua_Number>(comp.intensity));
  lua_pushnumber(state, static_cast<lua_Number>(comp.radius));
  return 5;
}

// Lua: engine.set_point_light(entity, r, g, b, intensity, radius) → boolean
static int lua_engine_set_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 6) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!runtime_binding().world->has_point_light_component(entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::PointLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 5));
  comp.radius = static_cast<float>(luaL_checknumber(state, 6));
  const bool ok = apply_or_queue_point_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.remove_point_light(entity) → boolean
static int lua_engine_remove_point_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_point_light_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// --- SpotLightComponent bindings ---

// Lua: engine.add_spot_light(entity, r, g, b, dx, dy, dz, intensity, radius,
//                            innerAngle, outerAngle) → boolean
static int lua_engine_add_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 11) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::SpotLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.direction.x = static_cast<float>(luaL_checknumber(state, 5));
  comp.direction.y = static_cast<float>(luaL_checknumber(state, 6));
  comp.direction.z = static_cast<float>(luaL_checknumber(state, 7));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 8));
  comp.radius = static_cast<float>(luaL_checknumber(state, 9));
  comp.innerConeAngle = static_cast<float>(luaL_checknumber(state, 10));
  comp.outerConeAngle = static_cast<float>(luaL_checknumber(state, 11));
  const bool ok = apply_or_queue_spot_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.get_spot_light(entity) → r, g, b, dx, dy, dz, intensity, radius,
//                                      innerAngle, outerAngle or nil
static int lua_engine_get_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    return 1;
  }
  runtime::SpotLightComponent comp{};
  if (!runtime_binding().world->get_spot_light_component(entity, &comp)) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.x));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.y));
  lua_pushnumber(state, static_cast<lua_Number>(comp.color.z));
  lua_pushnumber(state, static_cast<lua_Number>(comp.direction.x));
  lua_pushnumber(state, static_cast<lua_Number>(comp.direction.y));
  lua_pushnumber(state, static_cast<lua_Number>(comp.direction.z));
  lua_pushnumber(state, static_cast<lua_Number>(comp.intensity));
  lua_pushnumber(state, static_cast<lua_Number>(comp.radius));
  lua_pushnumber(state, static_cast<lua_Number>(comp.innerConeAngle));
  lua_pushnumber(state, static_cast<lua_Number>(comp.outerConeAngle));
  return 10;
}

// Lua: engine.set_spot_light(entity, r, g, b, dx, dy, dz, intensity, radius,
//                            innerAngle, outerAngle) → boolean
static int lua_engine_set_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 11) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!runtime_binding().world->has_spot_light_component(entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::SpotLightComponent comp{};
  comp.color.x = static_cast<float>(luaL_checknumber(state, 2));
  comp.color.y = static_cast<float>(luaL_checknumber(state, 3));
  comp.color.z = static_cast<float>(luaL_checknumber(state, 4));
  comp.direction.x = static_cast<float>(luaL_checknumber(state, 5));
  comp.direction.y = static_cast<float>(luaL_checknumber(state, 6));
  comp.direction.z = static_cast<float>(luaL_checknumber(state, 7));
  comp.intensity = static_cast<float>(luaL_checknumber(state, 8));
  comp.radius = static_cast<float>(luaL_checknumber(state, 9));
  comp.innerConeAngle = static_cast<float>(luaL_checknumber(state, 10));
  comp.outerConeAngle = static_cast<float>(luaL_checknumber(state, 11));
  const bool ok = apply_or_queue_spot_light_component(entity, comp);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

// Lua: engine.remove_spot_light(entity) → boolean
static int lua_engine_remove_spot_light(lua_State *state) noexcept {
  if (lua_gettop(state) < 1) {
    lua_pushboolean(state, 0);
    return 1;
  }
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = apply_or_queue_remove_spot_light_component(entity);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine start coroutine.
int lua_engine_start_coroutine(lua_State *state) noexcept {
  return start_lua_coroutine(state, g_totalSeconds, g_frameIndex,
                             log_lua_error);
}

// --- Entity lifecycle completeness ---

int lua_engine_find_by_name(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *searchName = lua_tostring(state, 1);
  if (searchName == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity found = runtime_binding().world->find_entity_by_name(searchName);

  if (found == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle(state, found);
  return 1;
}

/// Handles lua engine clone entity.
int lua_engine_clone_entity(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !can_apply_mutations_now()) {
    lua_pushnil(state);
    return 1;
  }
  runtime::Entity source{};
  if (!read_entity(state, 1, &source)) {
    lua_pushnil(state);
    return 1;
  }

  const runtime::Entity newEntity = runtime_binding().world->create_entity();
  if (newEntity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  // Copy Transform.
  runtime::Transform transform{};
  if (runtime_binding().world->get_transform(source, &transform)) {
    static_cast<void>(runtime_binding().world->add_transform(newEntity, transform));
  }

  // Copy RigidBody.
  runtime::RigidBody rigidBody{};
  if (runtime_binding().world->get_rigid_body(source, &rigidBody)) {
    static_cast<void>(runtime_binding().world->add_rigid_body(newEntity, rigidBody));
  }

  // Copy Collider.
  runtime::Collider collider{};
  if (runtime_binding().world->get_collider(source, &collider)) {
    static_cast<void>(runtime_binding().world->add_collider(newEntity, collider));
  }

  // Copy MeshComponent.
  runtime::MeshComponent mesh{};
  if (runtime_binding().world->get_mesh_component(source, &mesh)) {
    static_cast<void>(runtime_binding().world->add_mesh_component(newEntity, mesh));
  }

  // Copy NameComponent with "(clone)" suffix.
  runtime::NameComponent name{};
  if (runtime_binding().world->get_name_component(source, &name)) {
    runtime::NameComponent cloneName{};
    copy_clone_name(cloneName.name, sizeof(cloneName.name), name.name);
    static_cast<void>(runtime_binding().world->add_name_component(newEntity, cloneName));
  }

  // Copy LightComponent.
  runtime::LightComponent light{};
  if (runtime_binding().world->get_light_component(source, &light)) {
    static_cast<void>(runtime_binding().world->add_light_component(newEntity, light));
  }

  push_entity_handle(state, newEntity);
  return 1;
}

// --- Scene management from Lua (deferred load/new, immediate save) ---

enum class SceneOp : std::uint8_t { None, Load, New };
static SceneOp g_pendingSceneOp = SceneOp::None;
static char g_pendingScenePath[512] = {};

/// Handles lua engine save scene.
int lua_engine_save_scene(lua_State *state) noexcept {
  if (runtime_binding().world == nullptr || !lua_isstring(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = (runtime_binding().services != nullptr) && (runtime_binding().services->save_scene != nullptr)
                      ? runtime_binding().services->save_scene(runtime_binding().world, path)
                      : false;
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine load scene.
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

/// Handles lua engine new scene.
int lua_engine_new_scene(lua_State *state) noexcept {
  static_cast<void>(state);
  g_pendingSceneOp = SceneOp::New;
  return 0;
}

// --- Prefab bindings ---

int lua_engine_save_prefab(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isinteger(state, 1) ||
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
      (runtime_binding().services != nullptr) && (runtime_binding().services->save_prefab != nullptr)
          ? runtime_binding().services->save_prefab(runtime_binding().world, entity.index, path)
          : false;
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

/// Handles lua engine instantiate.
int lua_engine_instantiate(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isstring(state, 1)) {
    lua_pushnil(state);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  const std::uint32_t entityIndex =
      ((runtime_binding().services != nullptr) && (runtime_binding().services->instantiate_prefab != nullptr))
          ? runtime_binding().services->instantiate_prefab(runtime_binding().world, path)
          : 0U;
  if (entityIndex == 0U) {
    lua_pushnil(state);
    return 1;
  }
  push_entity_handle_from_index(state, entityIndex);
  return 1;
}

// --- Async asset streaming (P1-M4-C2c) ---

// engine.load_asset_async(path [, priority]) → handle_index or nil
// priority: 0=Low, 1=Normal, 2=High, 3=Immediate (default=Normal)
int lua_engine_load_asset_async(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->load_asset_async == nullptr)) {
    lua_pushnil(state);
    return 1;
  }
  if (!lua_isstring(state, 1)) {
    luaL_traceback(state, state, "load_asset_async: path must be a string", 1);
    core::log_message(core::LogLevel::Error, "scripting",
                      lua_tostring(state, -1));
    lua_pop(state, 1);
    lua_pushnil(state);
    return 1;
  }
  const char *path = lua_tostring(state, 1);
  if (path == nullptr) {
    lua_pushnil(state);
    return 1;
  }
  std::uint8_t priority = 1U; // Normal
  if (lua_isinteger(state, 2)) {
    const lua_Integer p = lua_tointeger(state, 2);
    if ((p >= 0) && (p <= 3)) {
      priority = static_cast<std::uint8_t>(p);
    }
  }
  const std::uint32_t handle = runtime_binding().services->load_asset_async(path, priority);
  if (handle == 0xFFFFFFFFU) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(handle));
  return 1;
}

// engine.is_asset_ready(handle_index) → boolean
int lua_engine_is_asset_ready(lua_State *state) noexcept {
  if ((runtime_binding().services == nullptr) || (runtime_binding().services->is_asset_ready == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  if (!lua_isinteger(state, 1)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const std::uint32_t handleIndex =
      static_cast<std::uint32_t>(lua_tointeger(state, 1));
  lua_pushboolean(state, runtime_binding().services->is_asset_ready(handleIndex) ? 1 : 0);
  return 1;
}

// --- Entity pool Lua bindings ---

// engine.pool_create(count) → pool_id or nil
int lua_engine_pool_create(lua_State *state) noexcept {
  if ((runtime_binding().world == nullptr) || !lua_isinteger(state, 1)) {
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
  if (!pool.init(runtime_binding().world, static_cast<std::size_t>(count))) {
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

  push_entity_handle(state, entity);
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

/// Handles lua engine add script component.
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

/// Handles lua engine remove script component.
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

/// Handles register engine bindings.
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

  lua_pushcfunction(state, &lua_engine_add_capsule_collider);
  lua_setfield(state, -2, "add_capsule_collider");

  lua_pushcfunction(state, &lua_engine_set_restitution);
  lua_setfield(state, -2, "set_restitution");

  lua_pushcfunction(state, &lua_engine_set_friction);
  lua_setfield(state, -2, "set_friction");

  // Physics materials (P1-M3-C1d).
  lua_pushcfunction(state, &lua_engine_create_physics_material);
  lua_setfield(state, -2, "create_physics_material");
  lua_pushcfunction(state, &lua_engine_set_collider_material);
  lua_setfield(state, -2, "set_collider_material");

  // Collision layers/masks (P1-M3-C2c).
  lua_pushcfunction(state, &lua_engine_set_collision_layer);
  lua_setfield(state, -2, "set_collision_layer");
  lua_pushcfunction(state, &lua_engine_set_collision_mask);
  lua_setfield(state, -2, "set_collision_mask");

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

  // Camera manager bindings (P1-M2-E).
  lua_pushcfunction(state, &lua_engine_push_camera);
  lua_setfield(state, -2, "push_camera");
  lua_pushcfunction(state, &lua_engine_pop_camera);
  lua_setfield(state, -2, "pop_camera");
  lua_pushcfunction(state, &lua_engine_get_active_camera);
  lua_setfield(state, -2, "get_active_camera");
  lua_pushcfunction(state, &lua_engine_camera_shake);
  lua_setfield(state, -2, "camera_shake");

  // Spring arm bindings (P1-M2-E).
  lua_pushcfunction(state, &lua_engine_add_spring_arm);
  lua_setfield(state, -2, "add_spring_arm");
  lua_pushcfunction(state, &lua_engine_get_spring_arm);
  lua_setfield(state, -2, "get_spring_arm");

  lua_pushcfunction(state, &lua_engine_set_gravity);
  lua_setfield(state, -2, "set_gravity");

  lua_pushcfunction(state, &lua_engine_get_gravity);
  lua_setfield(state, -2, "get_gravity");

  lua_pushcfunction(state, &lua_engine_raycast);
  lua_setfield(state, -2, "raycast");

  lua_pushcfunction(state, &lua_engine_raycast_all);
  lua_setfield(state, -2, "raycast_all");

  lua_pushcfunction(state, &lua_engine_overlap_sphere);
  lua_setfield(state, -2, "overlap_sphere");

  lua_pushcfunction(state, &lua_engine_overlap_box);
  lua_setfield(state, -2, "overlap_box");

  lua_pushcfunction(state, &lua_engine_sweep_sphere);
  lua_setfield(state, -2, "sweep_sphere");

  lua_pushcfunction(state, &lua_engine_sweep_box);
  lua_setfield(state, -2, "sweep_box");

  lua_pushcfunction(state, &lua_engine_add_distance_joint);
  lua_setfield(state, -2, "add_distance_joint");

  lua_pushcfunction(state, &lua_engine_add_hinge_joint);
  lua_setfield(state, -2, "add_hinge_joint");

  lua_pushcfunction(state, &lua_engine_add_ball_socket_joint);
  lua_setfield(state, -2, "add_ball_socket_joint");

  lua_pushcfunction(state, &lua_engine_add_slider_joint);
  lua_setfield(state, -2, "add_slider_joint");

  lua_pushcfunction(state, &lua_engine_add_spring_joint);
  lua_setfield(state, -2, "add_spring_joint");

  lua_pushcfunction(state, &lua_engine_add_fixed_joint);
  lua_setfield(state, -2, "add_fixed_joint");

  lua_pushcfunction(state, &lua_engine_set_joint_limits);
  lua_setfield(state, -2, "set_joint_limits");

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

  // PointLightComponent
  lua_pushcfunction(state, &lua_engine_add_point_light);
  lua_setfield(state, -2, "add_point_light");
  lua_pushcfunction(state, &lua_engine_get_point_light);
  lua_setfield(state, -2, "get_point_light");
  lua_pushcfunction(state, &lua_engine_set_point_light);
  lua_setfield(state, -2, "set_point_light");
  lua_pushcfunction(state, &lua_engine_remove_point_light);
  lua_setfield(state, -2, "remove_point_light");

  // SpotLightComponent
  lua_pushcfunction(state, &lua_engine_add_spot_light);
  lua_setfield(state, -2, "add_spot_light");
  lua_pushcfunction(state, &lua_engine_get_spot_light);
  lua_setfield(state, -2, "get_spot_light");
  lua_pushcfunction(state, &lua_engine_set_spot_light);
  lua_setfield(state, -2, "set_spot_light");
  lua_pushcfunction(state, &lua_engine_remove_spot_light);
  lua_setfield(state, -2, "remove_spot_light");

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
  lua_pushcfunction(state, &lua_engine_wait_frames);
  lua_setfield(state, -2, "wait_frames");
  lua_pushcfunction(state, &lua_engine_wait_until);
  lua_setfield(state, -2, "wait_until");

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

  // Async asset streaming (P1-M4-C2c)
  lua_pushcfunction(state, &lua_engine_load_asset_async);
  lua_setfield(state, -2, "load_asset_async");
  lua_pushcfunction(state, &lua_engine_is_asset_ready);
  lua_setfield(state, -2, "is_asset_ready");

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

  // Generated bindings override a curated subset of manual wrappers.
  register_generated_bindings(state);

  // Hot-reload state preservation.
  lua_pushcfunction(state, &lua_engine_persist);
  lua_setfield(state, -2, "persist");
  lua_pushcfunction(state, &lua_engine_restore);
  lua_setfield(state, -2, "restore");

  lua_setglobal(state, "engine");
}

// ---- Console cheat commands ----

void cmd_god(const char *const * /*args*/, int /*argCount*/,
             void * /*userData*/) noexcept {
  g_godModeEnabled = !g_godModeEnabled;
  core::console_print(g_godModeEnabled ? "God mode ON" : "God mode OFF");
}

/// Handles cmd noclip.
void cmd_noclip(const char *const * /*args*/, int /*argCount*/,
                void * /*userData*/) noexcept {
  g_noclipEnabled = !g_noclipEnabled;
  core::console_print(g_noclipEnabled ? "Noclip ON" : "Noclip OFF");
}

/// Handles cmd spawn.
void cmd_spawn(const char *const *args, int argCount,
               void * /*userData*/) noexcept {
  if (argCount < 2) {
    core::console_print("Usage: spawn <prefab> [x y z]");
    return;
  }
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->instantiate_prefab == nullptr)) {
    core::console_print("Cannot spawn: world not ready");
    return;
  }
  const std::uint32_t entityIndex =
      runtime_binding().services->instantiate_prefab(runtime_binding().world, args[1]);
  if (entityIndex == 0U) {
    core::console_print("Spawn failed (prefab not found?)");
    return;
  }
  // Optionally set position if x y z provided.
  if ((argCount >= 5) && (runtime_binding().services->add_transform_op != nullptr)) {
    runtime::Transform t{};
    t.position.x = static_cast<float>(std::atof(args[2]));
    t.position.y = static_cast<float>(std::atof(args[3]));
    t.position.z = static_cast<float>(std::atof(args[4]));
    t.scale = {1.0F, 1.0F, 1.0F};
    t.rotation = {0.0F, 0.0F, 0.0F, 1.0F};
    // Overwrite the transform that was loaded from the prefab.
    runtime_binding().services->add_transform_op(runtime_binding().world, entityIndex, t);
  }
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "Spawned entity %u", entityIndex);
  core::console_print(buf);
}

/// Handles cmd kill all.
void cmd_kill_all(const char *const * /*args*/, int /*argCount*/,
                  void * /*userData*/) noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr) ||
      (runtime_binding().services->destroy_entity_op == nullptr)) {
    core::console_print("Cannot kill_all: world not ready");
    return;
  }
  std::size_t destroyed = 0U;
  runtime_binding().world->for_each_alive([&destroyed](runtime::Entity entity) noexcept {
    if (is_player_controller_entity(entity)) {
      return;
    }
    runtime_binding().services->destroy_entity_op(runtime_binding().world, entity.index);
    ++destroyed;
  });
  char buf[64] = {};
  std::snprintf(buf, sizeof(buf), "Destroyed %zu entities", destroyed);
  core::console_print(buf);
}

/// Handles register debug commands.
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

/// Handles bindable delta time.
float bindable_delta_time() noexcept { return g_deltaSeconds; }

/// Handles bindable elapsed time.
float bindable_elapsed_time() noexcept { return g_totalSeconds; }

/// Handles bindable frame count.
int bindable_frame_count() noexcept { return static_cast<int>(g_frameIndex); }

/// Handles bindable get entity count.
int bindable_get_entity_count() noexcept {
  if ((runtime_binding().world == nullptr) || (runtime_binding().services == nullptr)) {
    return 0;
  }
  return static_cast<int>(runtime_binding().services->get_transform_count(runtime_binding().world));
}

/// Handles bindable is god mode.
bool bindable_is_god_mode() noexcept { return g_godModeEnabled; }

/// Handles bindable is noclip.
bool bindable_is_noclip() noexcept { return g_noclipEnabled; }

/// Handles bindable is gamepad connected.
bool bindable_is_gamepad_connected() noexcept {
  return core::is_gamepad_connected();
}

/// Handles bindable is key down.
bool bindable_is_key_down(int scancode) noexcept {
  return core::is_key_down(scancode);
}

/// Handles bindable is key pressed.
bool bindable_is_key_pressed(int scancode) noexcept {
  return core::is_key_pressed(scancode);
}

/// Handles bindable is gamepad button down.
bool bindable_is_gamepad_button_down(int button) noexcept {
  return core::is_gamepad_button_down(button);
}

/// Handles bindable is action down.
bool bindable_is_action_down(const char *name) noexcept {
  return (name != nullptr) ? core::is_action_down(name) : false;
}

/// Handles bindable is action pressed.
bool bindable_is_action_pressed(const char *name) noexcept {
  return (name != nullptr) ? core::is_action_pressed(name) : false;
}

/// Handles bindable get action value.
float bindable_get_action_value(const char *name) noexcept {
  return (name != nullptr) ? core::action_value(name) : 0.0F;
}

/// Handles bindable get axis value.
float bindable_get_axis_value(const char *name) noexcept {
  return (name != nullptr) ? core::axis_value(name) : 0.0F;
}

/// Handles bindable is alive.
bool bindable_is_alive(std::uint64_t entity) noexcept {
  if (runtime_binding().world == nullptr) {
    return false;
  }
  runtime::Entity decoded{};
  return decode_entity_handle_value(entity, &decoded) &&
         runtime_binding().world->is_alive(decoded);
}

/// Handles bindable has light.
bool bindable_has_light(std::uint64_t entity) noexcept {
  if (runtime_binding().world == nullptr) {
    return false;
  }
  runtime::Entity decoded{};
  if (!decode_entity_handle_value(entity, &decoded) ||
      !runtime_binding().world->is_alive(decoded)) {
    return false;
  }
  return runtime_binding().world->has_light_component(decoded);
}

/// Handles bindable set camera fov.
void bindable_set_camera_fov(float fov) noexcept {
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_camera_fov != nullptr)) {
    runtime_binding().services->set_camera_fov(fov);
  }
}

/// Handles bindable set master volume.
void bindable_set_master_volume(float volume) noexcept {
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->set_master_volume != nullptr)) {
    runtime_binding().services->set_master_volume(volume);
  }
}

/// Handles bindable stop all sounds.
void bindable_stop_all_sounds() noexcept {
  if ((runtime_binding().services != nullptr) && (runtime_binding().services->stop_all_sounds != nullptr)) {
    runtime_binding().services->stop_all_sounds();
  }
}

/// Initializes the owning system for scripting.
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
  set_debug_lua_state(g_state);
  configure_entity_script_bindings(
      g_state, EntityScriptBindingCallbacks{&push_entity_handle, &log_lua_error,
                                            &refresh_lua_hook,
                                            &get_file_mtime});

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

  // Install sandboxed memory allocator. io/os/debug libraries are already
  // excluded (only base, coroutine, table, string, math, utf8 are opened).
  if (debug_sandbox_enabled()) {
    lua_setallocf(g_state, sandbox_alloc, nullptr);
  }

  refresh_lua_hook();
  return true;
}

/// Shuts down the owning system for scripting.
void shutdown_scripting() noexcept {
  clear_touch_gesture_callbacks(g_state);

  if (g_state != nullptr) {
    clear_persist_bindings(g_state);
    reset_entity_script_bindings();
    clear_lua_timer_bindings(g_state);
    clear_collision_handlers(g_state);
    clear_lua_coroutines(g_state);
    lua_close(g_state);
    g_state = nullptr;
  }

  clear_runtime_binding();
  g_defaultMeshAssetId = 0ULL;
  g_builtinPlaneMesh = 0ULL;
  g_builtinCubeMesh = 0ULL;
  g_builtinSphereMesh = 0ULL;
  g_builtinCylinderMesh = 0ULL;
  g_builtinCapsuleMesh = 0ULL;
  g_builtinPyramidMesh = 0ULL;
  clear_deferred_mutations();
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
  reset_debug_bindings();
  set_debug_lua_state(nullptr);
  g_godModeEnabled = false;
  g_noclipEnabled = false;
  for (std::size_t i = 0U; i < kMaxEntityPools; ++i) {
    g_entityPools[i] = runtime::EntityPool{};
  }
  g_entityPoolCount = 0U;
  reset_game_bindings();
}

/// Sets the requested value for default mesh asset id.
void set_default_mesh_asset_id(std::uint64_t assetId) noexcept {
  g_defaultMeshAssetId = assetId;
}

/// Sets the requested value for builtin mesh ids.
void set_builtin_mesh_ids(std::uint64_t planeMesh, std::uint64_t cubeMesh,
                          std::uint64_t sphereMesh, std::uint64_t cylinderMesh,
                          std::uint64_t capsuleMesh,
                          std::uint64_t pyramidMesh) noexcept {
  g_builtinPlaneMesh = planeMesh;
  g_builtinCubeMesh = cubeMesh;
  g_builtinSphereMesh = sphereMesh;
  g_builtinCylinderMesh = cylinderMesh;
  g_builtinCapsuleMesh = capsuleMesh;
  g_builtinPyramidMesh = pyramidMesh;
}

/// Sets the requested value for frame time.
void set_frame_time(float deltaSeconds, float totalSeconds) noexcept {
  g_deltaSeconds = deltaSeconds;
  g_totalSeconds = totalSeconds;
  if (dap_is_running()) {
    dap_poll();
  }
}

/// Loads the requested resource for script.
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

  refresh_lua_hook(); // Reset instruction counter for this invocation.

  if (lua_pcall(g_state, 0, 0, 0) != LUA_OK) {
    log_lua_error("load_script");
    return false;
  }

  return true;
}

/// Handles call script function.
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

/// Handles call script function float.
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

/// Handles dispatch physics callbacks.
void dispatch_physics_callbacks(const std::uint32_t *pairData,
                                std::size_t pairCount) noexcept {
  dispatch_collision_handlers(g_state, pairData, pairCount,
                              push_entity_handle_from_index, log_lua_error);
}

namespace {
/// Returns the requested value for file mtime.
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
  // Use nanosecond-precision mtime when available so sub-second file writes
  // are detected (st_mtime alone has 1-second granularity on many POSIX FS).
#if defined(__APPLE__)
  return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL +
         static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#elif defined(__linux__)
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
         static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#else
  return static_cast<std::int64_t>(st.st_mtime);
#endif
#endif
}
} // anonymous namespace

/// Sets the requested value for frame index.
void set_frame_index(std::uint32_t frameIndex) noexcept {
  g_frameIndex = frameIndex;
}

/// Handles tick timers.
void tick_timers() noexcept {
  tick_lua_timers(g_state, g_deltaSeconds);
}

/// Handles tick coroutines.
void tick_coroutines() noexcept {
  tick_lua_coroutines(g_state, g_totalSeconds, g_frameIndex, log_lua_error,
                      refresh_lua_hook);
}

/// Handles clear coroutines.
void clear_coroutines() noexcept {
  clear_lua_coroutines(g_state);
}

/// Returns whether has pending scene op.
bool has_pending_scene_op() noexcept {
  return g_pendingSceneOp != SceneOp::None;
}

/// Handles pending scene op is load.
bool pending_scene_op_is_load() noexcept {
  return g_pendingSceneOp == SceneOp::Load;
}

/// Handles pending scene op is new.
bool pending_scene_op_is_new() noexcept {
  return g_pendingSceneOp == SceneOp::New;
}

/// Returns the requested value for pending scene path.
const char *get_pending_scene_path() noexcept { return g_pendingScenePath; }

/// Handles clear pending scene op.
void clear_pending_scene_op() noexcept {
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
}

/// Handles watch script file.
void watch_script_file(const char *path) noexcept {
  if (path == nullptr) {
    return;
  }
  copy_c_string(g_watchedPath, sizeof(g_watchedPath), path);
  g_watchedMtime = get_file_mtime(path);
}

/// Handles check script reload.
void check_script_reload() noexcept {
  if (g_watchedPath[0] == '\0') {
    return;
  }
  const std::int64_t mtime = get_file_mtime(g_watchedPath);
  if ((mtime != 0) && (mtime != g_watchedMtime)) {
    g_watchedMtime = mtime;
    core::log_message(core::LogLevel::Info, "scripting",
                      "hot-reloading script");
    if (!load_script(g_watchedPath)) {
      core::log_message(core::LogLevel::Warning, "scripting",
                        "hot-reload failed; keeping previous version");
    }
  }
}

// --- Sandbox configuration ---

void set_sandbox_enabled(bool enabled) noexcept {
  set_debug_sandbox_enabled(enabled);
  refresh_lua_hook();
}

/// Returns whether is sandbox enabled.
bool is_sandbox_enabled() noexcept { return debug_sandbox_enabled(); }

/// Sets the requested value for instruction limit.
void set_instruction_limit(int limit) noexcept {
  set_debug_instruction_limit(limit);
  refresh_lua_hook();
}

/// Returns the requested value for instruction limit.
int get_instruction_limit() noexcept { return debug_instruction_limit(); }

/// Sets the requested value for memory limit.
void set_memory_limit(std::size_t limit) noexcept { g_memoryLimit = limit; }

/// Returns the requested value for memory limit.
std::size_t get_memory_limit() noexcept { return g_memoryLimit; }

} // namespace engine::scripting
