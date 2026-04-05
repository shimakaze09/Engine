#include "engine/scripting/scripting.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstddef>
#include <cstdio>
#include <limits>

#include "engine/core/input.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/physics/physics.h"
#include "engine/renderer/camera.h"
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
std::uint32_t g_defaultMeshAssetId = 0U;
constexpr math::Vec3 kDefaultGravity(0.0F, -9.8F, 0.0F);
float g_deltaSeconds = 0.0F;
float g_totalSeconds = 0.0F;
std::uint32_t g_frameIndex = 0U;
char g_watchedPath[512] = {};
std::int64_t g_watchedMtime = 0;

bool read_entity_index(lua_State *state,
                       int index,
                       std::uint32_t *outIndex) noexcept {
  if ((outIndex == nullptr) || !lua_isnumber(state, index)) {
    return false;
  }

  const lua_Integer rawIndex = lua_tointeger(state, index);
  if ((rawIndex <= 0)
      || (rawIndex > static_cast<lua_Integer>(runtime::World::kMaxEntities))) {
    return false;
  }

  *outIndex = static_cast<std::uint32_t>(rawIndex);
  return true;
}

bool read_vec3_args(lua_State *state,
                    int startIndex,
                    math::Vec3 *outVec) noexcept {
  if ((outVec == nullptr) || !lua_isnumber(state, startIndex)
      || !lua_isnumber(state, startIndex + 1)
      || !lua_isnumber(state, startIndex + 2)) {
    return false;
  }

  const float x = static_cast<float>(lua_tonumber(state, startIndex));
  const float y = static_cast<float>(lua_tonumber(state, startIndex + 1));
  const float z = static_cast<float>(lua_tonumber(state, startIndex + 2));
  *outVec = math::Vec3(x, y, z);
  return true;
}

bool read_entity(lua_State *state,
                 int index,
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
    std::snprintf(
        logBuffer, sizeof(logBuffer), "lua error (%s): %s", context, message);
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
  const bool authoritySet = transformUpdated
                            && g_world->set_movement_authority(
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
    core::log_message(core::LogLevel::Warning,
                      "scripting",
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
  if (!read_entity(state, 1, &entity)
      || !read_vec3_args(state, 2, &acceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning,
                      "scripting",
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
  if (!read_entity(state, 1, &entity)
      || !read_vec3_args(state, 2, &additionalAcceleration)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::RigidBody rigidBody{};
  if (!g_world->get_rigid_body(entity, &rigidBody)) {
    core::log_message(core::LogLevel::Warning,
                      "scripting",
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

int lua_engine_set_mesh(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!read_entity(state, 1, &entity) || !lua_isnumber(state, 2)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  const lua_Integer meshId = lua_tointeger(state, 2);
  if ((meshId <= 0)
      || (meshId > static_cast<lua_Integer>(
              std::numeric_limits<std::uint32_t>::max()))) {
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
  if (!read_entity(state, 1, &entity)
      || !read_vec3_args(state, 2, &halfExtents)) {
    lua_pushboolean(state, 0);
    return 1;
  }

  runtime::Collider collider{};
  collider.halfExtents = halfExtents;

  const bool ok = g_world->add_collider(entity, collider);
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

int lua_engine_set_camera_position(lua_State *state) noexcept {
  math::Vec3 pos{};
  if (!read_vec3_args(state, 1, &pos)) {
    return 0;
  }
  renderer::CameraState cam = renderer::get_active_camera();
  cam.position = pos;
  renderer::set_active_camera(cam);
  return 0;
}

int lua_engine_set_camera_target(lua_State *state) noexcept {
  math::Vec3 target{};
  if (!read_vec3_args(state, 1, &target)) {
    return 0;
  }
  renderer::CameraState cam = renderer::get_active_camera();
  cam.target = target;
  renderer::set_active_camera(cam);
  return 0;
}

int lua_engine_set_camera_up(lua_State *state) noexcept {
  math::Vec3 up{};
  if (!read_vec3_args(state, 1, &up)) {
    return 0;
  }
  renderer::CameraState cam = renderer::get_active_camera();
  cam.up = up;
  renderer::set_active_camera(cam);
  return 0;
}

int lua_engine_set_camera_fov(lua_State *state) noexcept {
  if (!lua_isnumber(state, 1)) {
    return 0;
  }
  renderer::CameraState cam = renderer::get_active_camera();
  cam.fovRadians = static_cast<float>(lua_tonumber(state, 1));
  renderer::set_active_camera(cam);
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
  physics::set_gravity(x, y, z);
  return 0;
}

int lua_engine_get_gravity(lua_State *state) noexcept {
  const math::Vec3 g = physics::get_gravity();
  lua_pushnumber(state, static_cast<lua_Number>(g.x));
  lua_pushnumber(state, static_cast<lua_Number>(g.y));
  lua_pushnumber(state, static_cast<lua_Number>(g.z));
  return 3;
}

int lua_engine_frame_count(lua_State *state) noexcept {
  lua_pushinteger(state, static_cast<lua_Integer>(g_frameIndex));
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

  lua_pushcfunction(state, &lua_engine_frame_count);
  lua_setfield(state, -2, "frame_count");

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

  lua_setglobal(state, "engine");
}

} // namespace

bool initialize_scripting() noexcept {
  if (g_state != nullptr) {
    return true;
  }

  g_state = luaL_newstate();
  if (g_state == nullptr) {
    core::log_message(
        core::LogLevel::Error, "scripting", "failed to create Lua state");
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
    lua_close(g_state);
    g_state = nullptr;
  }

  g_world = nullptr;
  g_defaultMeshAssetId = 0U;
}

void set_scripting_world(runtime::World *world) noexcept {
  g_world = world;
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
    core::log_message(
        core::LogLevel::Error, "scripting", "scripting not initialized");
    return false;
  }

  if (path == nullptr) {
    core::log_message(
        core::LogLevel::Error, "scripting", "script path is null");
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
    core::log_message(
        core::LogLevel::Error, "scripting", "scripting not initialized");
    return false;
  }

  if (name == nullptr) {
    core::log_message(
        core::LogLevel::Error, "scripting", "script function name is null");
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
    core::log_message(
        core::LogLevel::Error, "scripting", "scripting not initialized");
    return false;
  }

  if (name == nullptr) {
    core::log_message(
        core::LogLevel::Error, "scripting", "script function name is null");
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

  lua_getglobal(g_state, "on_collision");
  if (!lua_isfunction(g_state, -1)) {
    lua_pop(g_state, 1);
    return;
  }
  lua_pop(g_state, 1);

  for (std::size_t i = 0U; i < pairCount; ++i) {
    lua_getglobal(g_state, "on_collision");
    if (!lua_isfunction(g_state, -1)) {
      lua_pop(g_state, 1);
      return;
    }
    lua_pushinteger(g_state, static_cast<lua_Integer>(pairData[i * 2U]));
    lua_pushinteger(g_state, static_cast<lua_Integer>(pairData[i * 2U + 1U]));
    if (lua_pcall(g_state, 2, 0, 0) != LUA_OK) {
      log_lua_error("on_collision");
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

void watch_script_file(const char *path) noexcept {
  if (path == nullptr) {
    return;
  }
  strncpy_s(
      g_watchedPath, sizeof(g_watchedPath), path, sizeof(g_watchedPath) - 1U);
  g_watchedMtime = get_file_mtime(path);
}

void check_script_reload() noexcept {
  if (g_watchedPath[0] == '\0') {
    return;
  }
  const std::int64_t mtime = get_file_mtime(g_watchedPath);
  if ((mtime != 0) && (mtime != g_watchedMtime)) {
    g_watchedMtime = mtime;
    core::log_message(
        core::LogLevel::Info, "scripting", "hot-reloading script");
    load_script(g_watchedPath);
  }
}
} // namespace engine::scripting
