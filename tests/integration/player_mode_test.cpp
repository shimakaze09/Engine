// Player mode (#138): ENGINE_PLAYER=1 must clear the editor bridge at
// bootstrap so the pipeline runs the pure gameplay loop (always-playing,
// scripts begin play with no editor present) and must arm
// r_present_scene so the renderer presents the final image on the back
// buffer — the web share page's boot path. Drives real
// engine::bootstrap() + a full EnginePipeline run with a pre-registered
// bridge that bootstrap has to displace.

#include "engine/core/cvar.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

constexpr const char *kScriptPath = "player_mode_test.lua";

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can author entities between
/// frames.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

/// Sentinel that must never run once bootstrap clears the bridge.
bool bridge_is_playing_never() noexcept { return false; }

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as pipeline_tick_cadence_test.cpp).
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};

  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }

    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/default.vert",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

// The probe runs through the production Lua dispatch path: on_begin_play
// only fires when the pipeline's loop state reports Playing, which with a
// cleared bridge must be the default.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.set_game_state(\"player_mode_running\")\n"
    "end\n"
    "return M\n";

bool write_script_file() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kScriptPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kScriptPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t length = std::char_traits<char>::length(kScript);
  const std::size_t written = std::fwrite(kScript, 1U, length, file);
  std::fclose(file);
  return written == length;
}

void remove_script_file() noexcept {
  static_cast<void>(std::remove(kScriptPath));
}

void set_player_env() noexcept {
#ifdef _WIN32
  static_cast<void>(_putenv_s("ENGINE_PLAYER", "1"));
#else
  static_cast<void>(setenv("ENGINE_PLAYER", "1", 1));
#endif
}

/// Runs one frame guaranteed to simulate at least one fixed step (see
/// pipeline_tick_cadence_test.cpp on the wall-clock accumulator).
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_script_file()) {
    std::fprintf(stderr, "FAIL: write script file\n");
    return 1;
  }

  // A bridge is registered before bootstrap exactly like the editor
  // library's static registration; player mode must displace it.
  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing_never;
  engine::runtime::set_editor_bridge(&bridge);

  set_player_env();

  // Full production bootstrap in headless mode (#196): the null render
  // device stands in so the pipeline runs on every CI lane.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_script_file();
    return 2;
  }

  CHECK(engine::runtime::editor_bridge() == nullptr,
        "player mode clears the editor bridge at bootstrap");
  CHECK(engine::core::cvar_get_bool("r_present_scene", false),
        "player mode arms r_present_scene");

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_script_file();
      return 3;
    }
    // With no bridge, capture_world never ran — the pipeline must expose
    // its world through its own accessor for the test to author into.
    g_world = pipeline.world();
    CHECK(g_world != nullptr, "pipeline world available without a bridge");

    CHECK(ticking_frame(pipeline), "settle frame 1");
    CHECK(ticking_frame(pipeline), "settle frame 2");
    if (g_world != nullptr) {
      const engine::runtime::Entity scripted = g_world->create_scene_object();
      CHECK(scripted != engine::runtime::kInvalidEntity, "spawn scripted");
      engine::runtime::ScriptComponent sc{};
      std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kScriptPath);
      CHECK(g_world->add_script_component(scripted, sc), "attach script");
      CHECK(ticking_frame(pipeline), "script frame 1");
      CHECK(ticking_frame(pipeline), "script frame 2");

      const char *state = engine::scripting::bindable_get_game_state();
      CHECK((state != nullptr) &&
                (std::strcmp(state, "player_mode_running") == 0),
            "scripts begin play with no editor present");
    }
    pipeline.teardown();
  }

  engine::shutdown();
  remove_script_file();

  if (g_failures != 0) {
    std::fprintf(stderr, "player_mode_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("player_mode_test: all checks passed\n");
  return 0;
}
