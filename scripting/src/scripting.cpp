#include "engine/scripting/scripting.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/math/quat.h"
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
constexpr math::Vec3 kDefaultGravity(0.0F, -9.8F, 0.0F);
float g_deltaSeconds = 0.0F;
float g_totalSeconds = 0.0F;
std::uint32_t g_frameIndex = 0U;
char g_watchedPath[512] = {};
std::int64_t g_watchedMtime = 0;

// --- Entity script module registry ---
struct EntityScriptModule final {
  char path[128] = {};
  int registryRef = LUA_NOREF;
};
constexpr std::size_t kMaxEntityScriptModules = 32U;
EntityScriptModule g_entityScriptModules[kMaxEntityScriptModules]{};
std::size_t g_entityScriptModuleCount = 0U;

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
  if ((g_world == nullptr) || (outEntity == nullptr)) {
    return false;
  }

  std::uint32_t entityIndex = 0U;
  if (!read_entity_index(state, index, &entityIndex)) {
    return false;
  }

  const runtime::Entity entity = g_world->find_entity_by_index(entityIndex);
  if (entity == runtime::kInvalidEntity) {
    return false;
  }

  *outEntity = entity;
  return true;
}

void log_lua_error(const char *context) noexcept {
  if (g_state == nullptr) {
    return;
  }

  const char *message = lua_tostring(g_state, -1);
  if (message == nullptr) {
    message = "unknown lua error";
  }

  char logBuffer[512] = {};
  if ((context != nullptr) && (context[0] != '\0')) {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error (%s): %s", context,
                  message);
  } else {
    std::snprintf(logBuffer, sizeof(logBuffer), "lua error: %s", message);
  }
  core::log_message(core::LogLevel::Error, "scripting", logBuffer);
  lua_pop(g_state, 1);
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
  const std::size_t count =
      (g_world != nullptr) ? g_world->transform_count() : 0U;
  lua_pushinteger(state, static_cast<lua_Integer>(count));
  return 1;
}

int lua_engine_spawn_entity(lua_State *state) noexcept {
  if (g_world == nullptr) {
    lua_pushnil(state);
    return 1;
  }

  // Only valid during Idle; create_entity returns kInvalidEntity otherwise.
  const runtime::Entity entity = g_world->create_entity();
  if (entity == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }

  lua_pushinteger(state, static_cast<lua_Integer>(entity.index));
  return 1;
}

int lua_engine_destroy_entity(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const bool ok = g_world->destroy_entity(entity);
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

  const runtime::Transform *transform = g_world->get_transform_read_ptr(entity);
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
  static_cast<void>(g_world->get_transform(entity, &transform));
  transform.position = position;

  const bool transformUpdated = g_world->add_transform(entity, transform);
  const bool authoritySet =
      transformUpdated && g_world->set_movement_authority(
                              entity, runtime::MovementAuthority::Script);
  const bool ok = transformUpdated && authoritySet;
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

  const bool ok = g_world->add_rigid_body(entity, rigidBody);
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

  const bool ok = g_world->add_rigid_body(entity, rigidBody);
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
  rigidBody.acceleration = math::sub(acceleration, kDefaultGravity);

  const bool ok = g_world->add_rigid_body(entity, rigidBody);
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

  const bool ok = g_world->add_rigid_body(entity, rigidBody);
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

  const bool ok = g_world->add_rigid_body(entity, rigidBody);
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
  const bool ok = g_world->add_mesh_component(entity, component);
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
  const bool ok = g_world->add_mesh_component(entity, component);
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
  std::snprintf(component.name, sizeof(component.name), "%s", name);
  component.name[sizeof(component.name) - 1U] = '\0';

  const bool ok = g_world->add_name_component(entity, component);
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
  if (!g_world->get_name_component(entity, &component)) {
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

  const bool ok = g_world->add_collider(entity, collider);
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
  runtime::Collider *col = g_world->get_collider_ptr(entity);
  if (col == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  col->restitution = value;
  lua_pushboolean(state, 1);
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
  runtime::Collider *col = g_world->get_collider_ptr(entity);
  if (col == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  col->staticFriction = staticF;
  col->dynamicFriction = dynamicF;
  lua_pushboolean(state, 1);
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
    g_services->set_gravity(x, y, z);
  }
  return 0;
}

int lua_engine_get_gravity(lua_State *state) noexcept {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  if ((g_services == nullptr) || (g_services->get_gravity == nullptr) ||
      !g_services->get_gravity(&x, &y, &z)) {
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
        static_cast<std::uint32_t>(lua_tointeger(state, 1)));
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
  const char *path = luaL_checkstring(state, 1);
  if ((g_services == nullptr) || (g_services->load_sound == nullptr)) {
    lua_pushinteger(state, 0);
    return 1;
  }
  lua_pushinteger(state,
                  static_cast<lua_Integer>(g_services->load_sound(path)));
  return 1;
}

int lua_engine_unload_sound(lua_State *state) noexcept {
  if ((g_services != nullptr) && (g_services->unload_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
    g_services->unload_sound(id);
  }
  return 0;
}

int lua_engine_play_sound(lua_State *state) noexcept {
  if ((g_services == nullptr) || (g_services->play_sound == nullptr)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
  float volume = 1.0F;
  float pitch = 1.0F;
  bool loop = false;
  if (lua_gettop(state) >= 2) {
    volume = static_cast<float>(luaL_optnumber(state, 2, 1.0));
  }
  if (lua_gettop(state) >= 3) {
    pitch = static_cast<float>(luaL_optnumber(state, 3, 1.0));
  }
  if (lua_gettop(state) >= 4) {
    loop = lua_toboolean(state, 4) != 0;
  }
  const bool ok = g_services->play_sound(id, volume, pitch, loop);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_stop_sound(lua_State *state) noexcept {
  if ((g_services != nullptr) && (g_services->stop_sound != nullptr)) {
    const auto id = static_cast<std::uint32_t>(luaL_checkinteger(state, 1));
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
  if ((g_services != nullptr) && (g_services->set_master_volume != nullptr)) {
    const auto vol = static_cast<float>(luaL_checknumber(state, 1));
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

  const bool transformUpdated = g_world->add_transform(entity, transform);
  const bool authoritySet =
      transformUpdated && g_world->set_movement_authority(
                              entity, runtime::MovementAuthority::Script);
  lua_pushboolean(state, (transformUpdated && authoritySet) ? 1 : 0);
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

  const bool transformUpdated = g_world->add_transform(entity, transform);
  const bool authoritySet =
      transformUpdated && g_world->set_movement_authority(
                              entity, runtime::MovementAuthority::Script);
  lua_pushboolean(state, (transformUpdated && authoritySet) ? 1 : 0);
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
  const bool ok = g_world->add_rigid_body(entity, rigidBody);
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
  runtime::Collider *col = g_world->get_collider_ptr(entity);
  if (col == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  col->halfExtents = halfExtents;
  lua_pushboolean(state, 1);
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
  runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh->roughness = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, 1);
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
  runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh->metallic = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, 1);
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
  runtime::MeshComponent *mesh = g_world->get_mesh_component_ptr(entity);
  if (mesh == nullptr) {
    lua_pushboolean(state, 0);
    return 1;
  }
  mesh->opacity = static_cast<float>(lua_tonumber(state, 2));
  lua_pushboolean(state, 1);
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
  const bool ok = g_world->add_light_component(entity, light);
  lua_pushboolean(state, ok ? 1 : 0);
  return 1;
}

int lua_engine_remove_light(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool ok = g_world->remove_light_component(entity);
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
  const bool ok = g_world->add_light_component(entity, light);
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
  const bool ok = g_world->add_light_component(entity, light);
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
  const bool ok = g_world->add_light_component(entity, light);
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

  runtime::Entity found = runtime::kInvalidEntity;
  g_world->for_each<runtime::NameComponent>(
      [&found, searchName](runtime::Entity e,
                           const runtime::NameComponent &nc) noexcept {
        if (std::strcmp(nc.name, searchName) == 0) {
          found = e;
        }
      });

  if (found == runtime::kInvalidEntity) {
    lua_pushnil(state);
    return 1;
  }
  lua_pushinteger(state, static_cast<lua_Integer>(found.index));
  return 1;
}

int lua_engine_clone_entity(lua_State *state) noexcept {
  if (g_world == nullptr) {
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
    std::snprintf(cloneName.name, sizeof(cloneName.name), "%s (clone)",
                  name.name);
    cloneName.name[sizeof(cloneName.name) - 1U] = '\0';
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

// Find an already-loaded entity script module by path, or load it fresh.
// Returns LUA_NOREF on failure. Must be called after log_lua_error is defined.
int get_or_load_entity_script_module(const char *path) noexcept {
  if ((g_state == nullptr) || (path == nullptr) || (path[0] == '\0')) {
    return LUA_NOREF;
  }

  for (std::size_t i = 0U; i < g_entityScriptModuleCount; ++i) {
    if (std::strcmp(g_entityScriptModules[i].path, path) == 0) {
      return g_entityScriptModules[i].registryRef;
    }
  }

  if (g_entityScriptModuleCount >= kMaxEntityScriptModules) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "entity script module limit reached");
    return LUA_NOREF;
  }

  if (luaL_loadfile(g_state, path) != LUA_OK) {
    log_lua_error("load entity script");
    return LUA_NOREF;
  }

  if (lua_pcall(g_state, 0, 1, 0) != LUA_OK) {
    log_lua_error("exec entity script");
    return LUA_NOREF;
  }

  if (!lua_istable(g_state, -1)) {
    core::log_message(core::LogLevel::Error, "scripting",
                      "entity script must return a module table");
    lua_pop(g_state, 1);
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
  ++g_entityScriptModuleCount;

  char logBuf[256] = {};
  std::snprintf(logBuf, sizeof(logBuf), "loaded entity script: %s", path);
  core::log_message(core::LogLevel::Info, "scripting", logBuf);
  return ref;
}

// Call module.funcName(entityIndex [, dt]) — returns false on missing/error.
bool call_module_function(int moduleRef, const char *funcName,
                          std::uint32_t entityIndex, bool hasDt,
                          float dt) noexcept {
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
    lua_pop(g_state, 2);
    return false;
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

  lua_pushboolean(state, g_world->add_script_component(entity, comp) ? 1 : 0);
  return 1;
}

int lua_engine_remove_script_component(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  lua_pushboolean(state, g_world->remove_script_component(entity) ? 1 : 0);
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
  g_pendingSceneOp = SceneOp::None;
  g_pendingScenePath[0] = '\0';
}

void bind_runtime_world(runtime::World *world) noexcept { g_world = world; }

void bind_runtime_services(const RuntimeServices *services) noexcept {
  g_services = services;
}

void set_default_mesh_asset_id(std::uint32_t assetId) noexcept {
  g_defaultMeshAssetId = assetId;
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
  strncpy_s(g_watchedPath, sizeof(g_watchedPath), path,
            sizeof(g_watchedPath) - 1U);
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
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
          return;
        }
        call_module_function(ref, "on_start", entity.index, false, 0.0F);
      });
}

void dispatch_entity_scripts_update(float dt) noexcept {
  if ((g_state == nullptr) || (g_world == nullptr)) {
    return;
  }

  g_world->for_each<runtime::ScriptComponent>(
      [dt](runtime::Entity entity,
           const runtime::ScriptComponent &sc) noexcept {
        if (sc.scriptPath[0] == '\0') {
          return;
        }
        const int ref = get_or_load_entity_script_module(sc.scriptPath);
        if (ref == LUA_NOREF) {
          return;
        }
        call_module_function(ref, "on_update", entity.index, true, dt);
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
  }
  g_entityScriptModuleCount = 0U;
}

} // namespace engine::scripting
