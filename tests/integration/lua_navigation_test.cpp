// Navigation end to end: a level is baked through the production bake and
// written as a .navmesh file in a directory of the test's own, and a real
// engine::bootstrap() and EnginePipeline in player mode load a scene whose
// NavMeshSurface names that file, through the production serializer. The level
// is a 20 by 20 m floor with a wall across x = 0 that is open past z = 4, and a
// platform 4 m off the floor's edge with nothing joining them. A script's
// on_fixed_tick asks engine.find_path for:
//   a. a path along the floor, from one end of the mesh to the other, on
//      the floor's surface;
//   b. a path from one side of the wall to the other, which goes around
//      its open end, longer than the straight line and past z = 4;
//   c. a path from off the mesh, refused as "off_mesh";
//   d. a path to the platform, refused as "unreachable";
//   e. a call with a non-number argument, refused as "invalid".
// Two runs, at 1 and 4 workers, report the same checks.

#include "../asset_root.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/navigation/nav_mesh.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/navigation_bake.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>

namespace {

constexpr const char *kScriptPath = "lua_navigation.lua";
constexpr const char *kScenePath = "lua_navigation.scene";
// The mesh is written to a directory of the test's own, mounted at its own
// prefix after bootstrap, never into the sample project's assets: other
// tests catalogue those in parallel, and would meet the file half written
// or half removed.
constexpr const char *kNavMeshDirectory = "lua_navigation_test_files";
constexpr const char *kNavMeshFile = "lua_navigation_test_files/level.navmesh";
constexpr const char *kNavMeshMount = "luanavtest";
constexpr const char *kNavMeshVirtualPath = "luanavtest/level.navmesh";
constexpr const char *kExpected = "abcde";

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

// The checks run once, on the tenth fixed step, after the pipeline has
// read the surface's file.
constexpr const char *kScript =
    "local M = {}\n"
    "local passed = {}\n"
    "local function expect(ok, letter)\n"
    "    if ok then passed[#passed + 1] = letter end\n"
    "end\n"
    "local function length(path)\n"
    "    local total = 0\n"
    "    for i = 2, #path do\n"
    "        local dx = path[i].x - path[i - 1].x\n"
    "        local dz = path[i].z - path[i - 1].z\n"
    "        total = total + math.sqrt(dx * dx + dz * dz)\n"
    "    end\n"
    "    return total\n"
    "end\n"
    "local ticks = 0\n"
    "function M.on_fixed_tick(self, dt)\n"
    "    ticks = ticks + 1\n"
    "    if ticks ~= 10 then return end\n"
    "    local along = engine.find_path(-6, 0, 6, 6, 0, 6)\n"
    "    local flat = along ~= nil and #along >= 2\n"
    "    if flat then\n"
    "        for _, p in ipairs(along) do\n"
    "            flat = flat and math.abs(p.y) < 0.01\n"
    "        end\n"
    "        flat = flat and math.abs(along[1].x + 6) < 0.3 and\n"
    "               math.abs(along[#along].x - 6) < 0.3\n"
    "    end\n"
    "    expect(flat, \"a\")\n"
    "    local around = engine.find_path(-6, 0, -6, 6, 0, -6)\n"
    "    local past_end = false\n"
    "    if around ~= nil then\n"
    "        for _, p in ipairs(around) do\n"
    "            if p.z > 4 then past_end = true end\n"
    "        end\n"
    "    end\n"
    "    expect(around ~= nil and #around >= 3 and past_end and\n"
    "           length(around) > 12.5, \"b\")\n"
    "    local none, why = engine.find_path(-60, 0, 0, 6, 0, 6)\n"
    "    expect(none == nil and why == \"off_mesh\", \"c\")\n"
    "    none, why = engine.find_path(-6, 0, 6, 16.5, 0, 0)\n"
    "    expect(none == nil and why == \"unreachable\", \"d\")\n"
    "    none, why = engine.find_path(\"x\", 0, 0, 6, 0, 6)\n"
    "    expect(none == nil and why == \"invalid\", \"e\")\n"
    "    engine.set_game_state(table.concat(passed))\n"
    "end\n"
    "return M\n";

bool write_file(const char *path, const void *bytes,
                std::size_t size) noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool wrote = std::fwrite(bytes, 1U, size, file) == size;
  return (std::fclose(file) == 0) && wrote;
}

void remove_fixtures() noexcept {
  static_cast<void>(std::remove(kScriptPath));
  static_cast<void>(std::remove(kScenePath));
  std::error_code ec{};
  std::filesystem::remove_all(kNavMeshDirectory, ec);
}

/// Adds a static box.
bool add_box(engine::runtime::World &world, const engine::math::Vec3 &position,
             const engine::math::Vec3 &halfExtents) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::Collider collider{};
  collider.halfExtents = halfExtents;
  return (entity != engine::runtime::kInvalidEntity) &&
         world.add_collider(entity, collider);
}

/// Authors the level, the surface over it and the scripted entity, bakes
/// the surface and writes its file, and saves the scene.
bool write_fixtures() noexcept {
  std::unique_ptr<engine::runtime::World> author(new (std::nothrow)
                                                     engine::runtime::World());
  if (author == nullptr) {
    return false;
  }
  using engine::math::Vec3;
  engine::runtime::Transform centre{};
  centre.position = Vec3(5.0F, 0.0F, 0.0F);
  const engine::runtime::Entity surfaceEntity =
      author->create_scene_object(centre);
  engine::runtime::NavMeshSurfaceComponent surface{};
  surface.halfExtents = Vec3(20.0F, 3.0F, 12.0F);
  std::snprintf(surface.navMeshPath, sizeof(surface.navMeshPath), "%s",
                kNavMeshVirtualPath);
  const engine::runtime::Entity scripted = author->create_scene_object();
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s",
                kScriptPath);
  if (!add_box(*author, Vec3(0.0F, -0.5F, 0.0F), Vec3(10.0F, 0.5F, 10.0F)) ||
      !add_box(*author, Vec3(0.0F, 1.0F, -3.0F), Vec3(0.5F, 1.0F, 7.0F)) ||
      !add_box(*author, Vec3(16.5F, -0.5F, 0.0F), Vec3(2.5F, 0.5F, 3.0F)) ||
      (surfaceEntity == engine::runtime::kInvalidEntity) ||
      !author->add_nav_mesh_surface(surfaceEntity, surface) ||
      (scripted == engine::runtime::kInvalidEntity) ||
      !author->add_script_component(scripted, script)) {
    return false;
  }
  engine::navigation::NavMesh mesh{};
  std::unique_ptr<std::uint8_t[]> bytes{};
  std::size_t size = 0U;
  std::error_code ec{};
  std::filesystem::create_directories(kNavMeshDirectory, ec);
  return engine::runtime::bake_nav_mesh_surface(*author, surfaceEntity,
                                                &mesh) &&
         !mesh.empty() &&
         engine::navigation::write_nav_mesh(mesh, &bytes, &size) &&
         write_file(kNavMeshFile, bytes.get(), size) &&
         write_file(kScriptPath, kScript, std::strlen(kScript)) &&
         engine::runtime::save_scene(*author, kScenePath);
}

void set_player_env() noexcept {
#ifdef _WIN32
  static_cast<void>(_putenv_s("ENGINE_CVAR_app_player_mode", "1"));
#else
  static_cast<void>(setenv("ENGINE_CVAR_app_player_mode", "1", 1));
#endif
}

struct RunResult final {
  bool ran = false;
  std::string state;
};

/// Runs the scene for 20 frames at `workers` workers.
RunResult run(std::uint32_t workers) noexcept {
  RunResult result{};
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.core.workerThreads = workers;
  config.editorScenePath = kScenePath;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap at %u workers\n", workers);
    return result;
  }
  if (!engine::core::mount(kNavMeshMount, kNavMeshDirectory)) {
    std::fprintf(stderr, "FAIL: mount the mesh's directory\n");
    engine::shutdown();
    return result;
  }
  {
    engine::EnginePipeline pipeline;
    if (pipeline.initialize(0U) &&
        pipeline.set_frame_delta_override(1.0 / 60.0)) {
      result.ran = true;
      for (int frame = 0; frame < 20; ++frame) {
        result.ran = pipeline.execute_frame() && result.ran;
      }
      const char *state = engine::scripting::bindable_get_game_state();
      result.state = (state != nullptr) ? state : "";
    }
    pipeline.teardown();
  }
  engine::shutdown();
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_fixtures()) {
    std::fprintf(stderr, "FAIL: write the level, its mesh and the script\n");
    remove_fixtures();
    return 1;
  }
  set_player_env();

  const RunResult one = run(1U);
  const RunResult four = run(4U);
  CHECK(one.ran && four.ran, "both runs execute every frame");
  CHECK(one.state == kExpected,
        "every navigation check in the script passed (1 worker)");
  if (one.state != kExpected) {
    std::fprintf(stderr, "  passed: \"%s\"\n", one.state.c_str());
  }
  CHECK(four.state == one.state, "both runs report the same checks");
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_navigation_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("lua_navigation_test: all checks passed\n");
  return 0;
}
