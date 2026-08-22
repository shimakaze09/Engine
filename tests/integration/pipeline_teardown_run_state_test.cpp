// Regression for #168 M2: EnginePipeline::teardown must reset run-scoped
// state so a later pipeline run in the same process starts clean. Before
// this fix the scripting run state (game-state label and friends) and the
// animation controller registry were reset only by engine::shutdown, editor
// Stop's VM recycle, or a scene transition — plain teardown leaked them
// into the next run. Drives real engine::bootstrap() + two full
// EnginePipeline runs (the pipeline_tick_cadence_test.cpp pattern): run A
// mutates game state from an entity script and loads the bootstrap
// character's animation controller, teardown must clear both, and run B
// must start from defaults.

#include "engine/audio/audio.h"
#include "engine/core/input.h"
#include "engine/engine.h"
#include "engine/renderer/command_buffer.h"
#include "engine/runtime/animation_system.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace {

constexpr const char *kScriptPath = "pipeline_teardown_run_state_test.lua";

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can author entities between frames.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

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
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

// The mutation runs through the production Lua dispatch path: the script's
// on_begin_play marks the run by moving the game-state label off its
// "startup" default.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    engine.set_game_state(\"in_progress\")\n"
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

/// Runs one playing frame guaranteed to simulate at least one fixed step
/// (see pipeline_tick_cadence_test.cpp on the wall-clock accumulator).
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

/// Compares the live game-state label against an expected value.
bool game_state_is(const char *expected) noexcept {
  const char *actual = engine::scripting::bindable_get_game_state();
  return (actual != nullptr) && (std::strcmp(actual, expected) == 0);
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

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  if (!engine::bootstrap()) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_script_file();
    return 2;
  }

  // --- Run A: dirty the run-scoped state through production paths. ---
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline A initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_script_file();
      return 3;
    }

    // Settle so the bootstrap scene begins play (its animated character
    // acquires a controller slot), then let the test script's
    // on_begin_play move the game state off its default.
    CHECK(ticking_frame(pipeline), "run A settle frame 1");
    CHECK(ticking_frame(pipeline), "run A settle frame 2");
    const engine::runtime::Entity scripted = g_world->create_scene_object();
    CHECK(scripted != engine::runtime::kInvalidEntity, "spawn scripted");
    engine::runtime::ScriptComponent sc{};
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kScriptPath);
    CHECK(g_world->add_script_component(scripted, sc), "attach script");
    CHECK(ticking_frame(pipeline), "run A script frame 1");
    CHECK(ticking_frame(pipeline), "run A script frame 2");

    // Engine-tier run content (#168 M2b): a gameplay input binding, a
    // loaded scene sound, and rendered-frame renderer state.
    CHECK(engine::core::register_action("m2b_probe_action", 44),
          "register probe action");
    CHECK(engine::core::register_axis("m2b_probe_axis", 4, 7),
          "register probe axis");
    const engine::audio::SoundHandle probeSound =
        engine::audio::load_sound("assets/sounds/pickup.wav");
    CHECK(probeSound != engine::audio::kInvalidSound, "load probe sound");
    CHECK(engine::audio::play_sound(probeSound, {}), "probe sound plays");
    CHECK(ticking_frame(pipeline), "run A rendered frame");

    // Guard asserts: the run really dirtied every probe before teardown.
    CHECK(game_state_is("in_progress"), "run A moved the game state");
    CHECK(engine::runtime::get_anim_controller(0U) != nullptr,
          "run A acquired an animation controller slot");
    CHECK(engine::core::gameplay_action_count() > 0U, "actions registered");
    CHECK(engine::core::gameplay_axis_count() > 0U, "axes registered");
    CHECK(engine::renderer::renderer_get_last_frame_stats().drawCalls > 0U,
          "run A rendered draw calls");

    pipeline.teardown();

    // The regression proper: teardown alone must clear run residue.
    CHECK(game_state_is("startup"),
          "teardown resets the game-state label to its default");
    CHECK(engine::runtime::get_anim_controller(0U) == nullptr,
          "teardown releases the animation controller registry");
    CHECK(engine::core::gameplay_action_count() == 0U,
          "teardown clears gameplay actions");
    CHECK(engine::core::gameplay_axis_count() == 0U,
          "teardown clears gameplay axes");
    CHECK(!engine::audio::play_sound(probeSound, {}),
          "teardown unloaded the scene sound");
    CHECK(engine::renderer::renderer_get_last_frame_stats().drawCalls == 0U,
          "teardown resets the public renderer state");
  }

  // --- Run B: a second run in the same process starts and stays clean. ---
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline B initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_script_file();
      return 4;
    }

    CHECK(game_state_is("startup"), "run B starts from the default state");
    CHECK(ticking_frame(pipeline), "run B ticking frame");
    pipeline.teardown();
    CHECK(engine::runtime::get_anim_controller(0U) == nullptr,
          "run B teardown also releases controllers");
  }

  engine::shutdown();
  remove_script_file();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("pipeline_teardown_run_state_test passed");
  return 0;
}
