// Implements the navigation Lua bindings: engine.find_path over the
// meshes the scene's NavMeshSurface components baked.

#include "navigation_bindings.h"

#include "binding_util.h"
#include "runtime_binding.h"

#include "lua.h"

#include <cstddef>

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
  if (!read_vec3_args(state, 1, &start) || !read_vec3_args(state, 4, &end)) {
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

} // namespace

void register_navigation_bindings(lua_State *state) noexcept {
  lua_pushcfunction(state, &lua_engine_find_path);
  lua_setfield(state, -2, "find_path");
}

} // namespace engine::scripting
