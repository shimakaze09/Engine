// Navigation agents end to end: a level is baked through the production
// bake and written as a .navmesh file in a directory of the test's own, and
// a real engine::bootstrap() and EnginePipeline in player mode load a scene,
// saved through the production serializer, whose NavMeshSurface names that file
// and whose two entities carry Nav Agents. The level is a 20 by 20 m floor with
// a wall across x = 0 that is open past z = 4, and a platform 4 m off the
// floor's edge with nothing joining them. A script:
//   a. reads the walker as "idle" before it is sent anywhere;
//   b. sends it around the wall with engine.set_nav_destination;
//   c. sends the other agent to the platform;
//   d. reads "no_agent" for an entity without one;
//   e. is refused sending an entity without an agent;
//   f. sees the walker moving and then arrived;
//   g. finds it within its stopping distance of the destination;
//   h. reads the other agent "failed" as "unreachable";
//   i. finds the walker never moved faster than its speed.
// Two runs, at 1 and 4 workers, report the same checks and leave the
// walker at the same position, bit for bit.

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

constexpr const char *kScriptPath = "lua_nav_agent.lua";
constexpr const char *kScenePath = "lua_nav_agent.scene";
// The mesh is written to a directory of the test's own, mounted at its own
// prefix after bootstrap, never into the sample project's assets: other
// tests catalogue those in parallel, and would meet the file half written
// or half removed.
constexpr const char *kNavMeshDirectory = "lua_nav_agent_test_files";
constexpr const char *kNavMeshFile = "lua_nav_agent_test_files/level.navmesh";
constexpr const char *kNavMeshMount = "luaagenttest";
constexpr const char *kNavMeshVirtualPath = "luaagenttest/level.navmesh";
constexpr const char *kExpected = "abcdefghi";
/// Frames each run lasts: the walker's 23 m around the wall at 6 m/s
/// takes about 250 fixed steps, and the script reports on step 400.
constexpr int kFrames = 410;

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

constexpr const char *kScript =
    "local M = {}\n"
    "local passed = {}\n"
    "local function expect(ok, letter)\n"
    "    if ok then passed[#passed + 1] = letter end\n"
    "end\n"
    "local ticks = 0\n"
    "local walker, stranded, manager\n"
    "local sawMoving, arrivedAt = false, nil\n"
    "local lastX, lastZ = nil, nil\n"
    "local fastest = 0\n"
    "function M.on_fixed_tick(self, dt)\n"
    "    ticks = ticks + 1\n"
    "    if ticks == 1 then\n"
    "        walker = engine.find_entity_by_name(\"Walker\")\n"
    "        stranded = engine.find_entity_by_name(\"Stranded\")\n"
    "        manager = engine.find_entity_by_name(\"Manager\")\n"
    "        expect(engine.nav_agent_status(walker) == \"idle\", \"a\")\n"
    "        expect(engine.set_nav_destination(walker, 6, 0, -6), \"b\")\n"
    "        expect(engine.set_nav_destination(stranded, 16.5, 0, 0), \"c\")\n"
    "        local none, why = engine.nav_agent_status(manager)\n"
    "        expect(none == nil and why == \"no_agent\", \"d\")\n"
    "        expect(engine.set_nav_destination(manager, 0, 0, 0) == false,\n"
    "               \"e\")\n"
    "        return\n"
    "    end\n"
    "    local status = engine.nav_agent_status(walker)\n"
    "    if status == \"moving\" then sawMoving = true end\n"
    "    if status == \"arrived\" and arrivedAt == nil then\n"
    "        arrivedAt = ticks\n"
    "    end\n"
    "    local x, _, z = engine.get_position(walker)\n"
    "    if lastX ~= nil then\n"
    "        local dx, dz = x - lastX, z - lastZ\n"
    "        local step = math.sqrt(dx * dx + dz * dz)\n"
    "        if step > fastest then fastest = step end\n"
    "    end\n"
    "    lastX, lastZ = x, z\n"
    "    if ticks ~= 400 then return end\n"
    "    expect(sawMoving and arrivedAt ~= nil, \"f\")\n"
    "    local dx, dz = x - 6, z + 6\n"
    "    expect(math.sqrt(dx * dx + dz * dz) <= 0.1 + 1e-4, \"g\")\n"
    "    local failed, why = engine.nav_agent_status(stranded)\n"
    "    expect(failed == \"failed\" and why == \"unreachable\", \"h\")\n"
    "    expect(fastest <= 6 * dt + 1e-4, \"i\")\n"
    "    local _, y = engine.get_position(walker)\n"
    "    engine.set_game_state(table.concat(passed) ..\n"
    "        string.format(\"|%.9g,%.9g,%.9g|%d\", x, y, z, arrivedAt or -1))\n"
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

/// Creates a scene object named `name` at `position`.
engine::runtime::Entity add_named(engine::runtime::World &world,
                                  const char *name,
                                  const engine::math::Vec3 &position) noexcept {
  engine::runtime::Transform transform{};
  transform.position = position;
  const engine::runtime::Entity entity = world.create_scene_object(transform);
  engine::runtime::NameComponent nameComponent{};
  std::snprintf(nameComponent.name, sizeof(nameComponent.name), "%s", name);
  return ((entity != engine::runtime::kInvalidEntity) &&
          world.add_name_component(entity, nameComponent))
             ? entity
             : engine::runtime::kInvalidEntity;
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

/// Authors the level, the surface over it, the two agents and the
/// scripted manager, bakes the surface and writes its file, and saves the
/// scene.
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
  engine::runtime::NavAgentComponent agent{};
  agent.speed = 6.0F;
  agent.acceleration = 12.0F;
  const engine::runtime::Entity walker =
      add_named(*author, "Walker", Vec3(-6.0F, 0.0F, -6.0F));
  const engine::runtime::Entity stranded =
      add_named(*author, "Stranded", Vec3(-6.0F, 0.0F, 6.0F));
  const engine::runtime::Entity manager =
      add_named(*author, "Manager", Vec3(0.0F, 0.0F, 0.0F));
  engine::runtime::ScriptComponent script{};
  std::snprintf(script.behaviours[0].scriptPath, sizeof(script.behaviours[0].scriptPath), "%s",
                kScriptPath);
  if (!add_box(*author, Vec3(0.0F, -0.5F, 0.0F), Vec3(10.0F, 0.5F, 10.0F)) ||
      !add_box(*author, Vec3(0.0F, 1.0F, -3.0F), Vec3(0.5F, 1.0F, 7.0F)) ||
      !add_box(*author, Vec3(16.5F, -0.5F, 0.0F), Vec3(2.5F, 0.5F, 3.0F)) ||
      (surfaceEntity == engine::runtime::kInvalidEntity) ||
      !author->add_nav_mesh_surface(surfaceEntity, surface) ||
      (walker == engine::runtime::kInvalidEntity) ||
      !author->add_nav_agent(walker, agent) ||
      (stranded == engine::runtime::kInvalidEntity) ||
      !author->add_nav_agent(stranded, agent) ||
      (manager == engine::runtime::kInvalidEntity) ||
      !author->add_script_component(manager, script)) {
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

/// Runs the scene for kFrames frames at `workers` workers.
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
      for (int frame = 0; frame < kFrames; ++frame) {
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
  const std::string passed = one.state.substr(0, one.state.find('|'));
  CHECK(passed == kExpected,
        "every agent check in the script passed (1 worker)");
  std::printf("lua_nav_agent_test: 1 worker \"%s\", 4 workers \"%s\"\n",
              one.state.c_str(), four.state.c_str());
  CHECK(four.state == one.state,
        "both runs pass the same checks and leave the walker at the same "
        "position, bit for bit");
  remove_fixtures();

  if (g_failures != 0) {
    std::fprintf(stderr, "lua_nav_agent_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("lua_nav_agent_test: all checks passed\n");
  return 0;
}
