// Implements the navigation Lua bindings: engine.find_path over the
// meshes the scene's NavMeshSurface components baked, and the calls that
// send a Nav Agent to a destination and read how it is doing.

#include "navigation_bindings.h"

#include "binding_util.h"
#include "entity_handle.h"
#include "runtime_binding.h"

#include "lua.h"

#include <cstddef>

#include "engine/core/logging.h"
#include "engine/math/vec3.h"
#include "engine/scripting/runtime_services.h"

namespace engine::scripting {

namespace {

/// The most corners one path hands a script.
constexpr std::size_t kMaxPathCorners = 256U;

/// Why a path was not found, as find_path's second result names it.
const char *path_failure_name(RuntimePathResult result) noexcept {
  switch (result) {
  case RuntimePathResult::Found:
    return "found";
  case RuntimePathResult::OffMesh:
    return "off_mesh";
  case RuntimePathResult::Unreachable:
    return "unreachable";
  case RuntimePathResult::TooLong:
    return "too_long";
  }
  return "off_mesh";
}

// engine.find_path(sx, sy, sz, ex, ey, ez) → { {x=,y=,z=}, ... } | nil, why
// The corners from the start, snapped onto the mesh whose bake volume
// holds it, to the end, snapped too. On failure nil and "off_mesh",
// "unreachable" or "too_long" (more than 256 corners), or "invalid" for an
// argument that is not a finite number.
int lua_engine_find_path(lua_State *state) noexcept {
  math::Vec3 start{};
  math::Vec3 end{};
  int arg = 1;
  if (!read_vec3_arg(state, &arg, &start) || !read_vec3_arg(state, &arg, &end)) {
    lua_pushnil(state);
    lua_pushstring(state, "invalid");
    return 2;
  }
  if (!runtime_bound() ||
      (runtime_binding().services->find_path_op == nullptr)) {
    lua_pushnil(state);
    lua_pushstring(state, "off_mesh");
    return 2;
  }
  RuntimePathPoint corners[kMaxPathCorners];
  std::size_t count = 0U;
  const RuntimePathResult result = runtime_binding().services->find_path_op(
      runtime_binding().world, start.x, start.y, start.z, end.x, end.y,
      end.z, corners, kMaxPathCorners, &count);
  if (result != RuntimePathResult::Found) {
    lua_pushnil(state);
    lua_pushstring(state, path_failure_name(result));
    return 2;
  }
  lua_createtable(state, static_cast<int>(count), 0);
  for (std::size_t i = 0U; i < count; ++i) {
    lua_createtable(state, 0, 3);
    lua_pushnumber(state, static_cast<lua_Number>(corners[i].x));
    lua_setfield(state, -2, "x");
    lua_pushnumber(state, static_cast<lua_Number>(corners[i].y));
    lua_setfield(state, -2, "y");
    lua_pushnumber(state, static_cast<lua_Number>(corners[i].z));
    lua_setfield(state, -2, "z");
    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1U));
  }
  return 1;
}

/// An agent's status as nav_agent_status names it.
const char *agent_status_name(RuntimeNavAgentStatus status) noexcept {
  switch (status) {
  case RuntimeNavAgentStatus::Idle:
    return "idle";
  case RuntimeNavAgentStatus::Pending:
    return "pending";
  case RuntimeNavAgentStatus::Moving:
    return "moving";
  case RuntimeNavAgentStatus::Arrived:
    return "arrived";
  case RuntimeNavAgentStatus::Failed:
    return "failed";
  }
  return "idle";
}

/// Why an agent failed, as nav_agent_status's second result names it.
const char *agent_failure_name(RuntimeNavAgentFailure failure) noexcept {
  switch (failure) {
  case RuntimeNavAgentFailure::OffMesh:
    return "off_mesh";
  case RuntimeNavAgentFailure::Unreachable:
    return "unreachable";
  case RuntimeNavAgentFailure::TooLong:
    return "too_long";
  case RuntimeNavAgentFailure::CannotMove:
    return "cannot_move";
  case RuntimeNavAgentFailure::None:
    break;
  }
  return "none";
}

// engine.set_nav_destination(entity, x, y, z) → bool: sends the entity's
// Nav Agent to the point; its path is found on its next fixed step and it
// walks there at its speed. False, with a logged reason, when the entity
// has no Nav Agent, is parented, or an argument is not a finite number.
int lua_engine_set_nav_destination(lua_State *state) noexcept {
  runtime::Entity entity{};
  math::Vec3 destination{};
  int vectorArg = 2;
  if (!runtime_bound() || !read_entity(state, 1, &entity) ||
      !read_vec3_arg(state, &vectorArg, &destination)) {
    core::log_message(core::LogLevel::Warning, "scripting",
                      "set_nav_destination takes an entity and three finite "
                      "numbers");
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool sent =
      (runtime_binding().services->set_nav_destination_op != nullptr) &&
      runtime_binding().services->set_nav_destination_op(
          runtime_binding().world, entity, destination.x, destination.y,
          destination.z);
  lua_pushboolean(state, sent ? 1 : 0);
  return 1;
}

// engine.stop_nav_agent(entity) → bool: stops the entity's Nav Agent where
// it stands; false when it has none.
int lua_engine_stop_nav_agent(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!runtime_bound() || !read_entity(state, 1, &entity)) {
    lua_pushboolean(state, 0);
    return 1;
  }
  const bool stopped =
      (runtime_binding().services->stop_nav_agent_op != nullptr) &&
      runtime_binding().services->stop_nav_agent_op(runtime_binding().world,
                                                    entity);
  lua_pushboolean(state, stopped ? 1 : 0);
  return 1;
}

// engine.nav_agent_status(entity) → status, detail: "idle", "pending",
// "moving" or "arrived" with the path length left to the destination, or
// "failed" with why: "off_mesh", "unreachable", "too_long" or
// "cannot_move". Nil and "no_agent" when the entity has no Nav Agent, nil
// and "invalid" for an argument that is not an entity.
int lua_engine_nav_agent_status(lua_State *state) noexcept {
  runtime::Entity entity{};
  if (!runtime_bound() || !read_entity(state, 1, &entity)) {
    lua_pushnil(state);
    lua_pushstring(state, "invalid");
    return 2;
  }
  RuntimeNavAgentState agent{};
  if ((runtime_binding().services->nav_agent_state_op == nullptr) ||
      !runtime_binding().services->nav_agent_state_op(runtime_binding().world,
                                                      entity, &agent)) {
    lua_pushnil(state);
    lua_pushstring(state, "no_agent");
    return 2;
  }
  lua_pushstring(state, agent_status_name(agent.status));
  if (agent.status == RuntimeNavAgentStatus::Failed) {
    lua_pushstring(state, agent_failure_name(agent.failure));
  } else {
    lua_pushnumber(state, static_cast<lua_Number>(agent.remainingDistance));
  }
  return 2;
}

} // namespace

void register_navigation_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_find_path);
  lua_setfield(state, -2, "find_path");
  lua_pushcfunction(state, &lua_engine_set_nav_destination);
  lua_setfield(state, -2, "set_nav_destination");
  lua_pushcfunction(state, &lua_engine_stop_nav_agent);
  lua_setfield(state, -2, "stop_nav_agent");
  lua_pushcfunction(state, &lua_engine_nav_agent_status);
  lua_setfield(state, -2, "nav_agent_status");
}

} // namespace engine::scripting
