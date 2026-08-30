// Regression for #345: the Lua-visible clocks (engine.delta_time /
// engine.elapsed_time / engine.frame_count) are run-scoped, but they lived in
// scripting globals that no run boundary cleared, and the pipeline published
// them only in stage_scripting — after stage_play_transitions has already
// dispatched begin-play/start callbacks. A second pipeline run's
// on_begin_play therefore observed the previous run's clock values. Drives
// real engine::bootstrap() + two full EnginePipeline runs (the
// pipeline_teardown_run_state_test.cpp pattern, headless): run A advances the
// clocks, teardown must zero them, and run B's first-frame on_begin_play —
// which fires before run B ever publishes a frame time — must read all three
// clocks as zero through the production Lua dispatch path.

#include "engine/engine.h"
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

constexpr const char *kScriptPath = "pipeline_clock_reset_test.lua";

engine::runtime::World *g_world = nullptr;

/// Captures the pipeline's world so the test can author entities before the
/// first frame.
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

// The probe captures the clocks at the exact dispatch the finding names:
// on_begin_play, which stage_play_transitions runs ahead of stage_scripting's
// per-frame time publication. The observation is recorded through the
// game-state label so the C++ side reads what the callback saw, not what a
// later stage republished.
constexpr const char *kScript =
    "local M = {}\n"
    "function M.on_begin_play(self)\n"
    "    local d = engine.delta_time()\n"
    "    local e = engine.elapsed_time()\n"
    "    local f = engine.frame_count()\n"
    "    if d == 0 and e == 0 and f == 0 then\n"
    "        engine.set_game_state(\"clocks_zero\")\n"
    "    else\n"
    "        print(string.format(\"begin_play clocks d=%g e=%g f=%d\", d, e, "
    "f))\n"
    "        engine.set_game_state(\"clocks_leaked\")\n"
    "    end\n"
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

  // Full production bootstrap in headless mode (#196): the null render
  // device stands in so the pipeline runs on every CI lane.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_script_file();
    return 2;
  }

  // --- Run A: advance the clocks through real playing frames. ---
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline A initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_script_file();
      return 3;
    }

    CHECK(ticking_frame(pipeline), "run A frame 1");
    CHECK(ticking_frame(pipeline), "run A frame 2");
    CHECK(ticking_frame(pipeline), "run A frame 3");

    // Guard asserts: the run really advanced every clock, otherwise run B's
    // zero-observation would prove nothing.
    CHECK(engine::scripting::bindable_delta_time() > 0.0F,
          "run A published a nonzero delta");
    CHECK(engine::scripting::bindable_elapsed_time() > 0.0F,
          "run A published a nonzero elapsed time");
    CHECK(engine::scripting::bindable_frame_count() > 0,
          "run A published a nonzero frame index");

    pipeline.teardown();

    // The run boundary itself must zero the clocks: an embedder reading them
    // between runs (or a run B that never simulates) must not see run A time.
    CHECK(engine::scripting::bindable_delta_time() == 0.0F,
          "teardown zeroes the published delta");
    CHECK(engine::scripting::bindable_elapsed_time() == 0.0F,
          "teardown zeroes the published elapsed time");
    CHECK(engine::scripting::bindable_frame_count() == 0,
          "teardown zeroes the published frame index");
  }

  // --- Run B: the first begin-play dispatch observes the new run's clocks. ---
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline B initialize\n");
      pipeline.teardown();
      engine::shutdown();
      remove_script_file();
      return 4;
    }

    // Authored before the first frame so on_begin_play fires during run B's
    // first stage_play_transitions — ahead of run B's first stage_scripting
    // publication, the exact window the finding describes.
    const engine::runtime::Entity scripted = g_world->create_scene_object();
    CHECK(scripted != engine::runtime::kInvalidEntity, "spawn scripted");
    engine::runtime::ScriptComponent sc{};
    std::snprintf(sc.scriptPath, sizeof(sc.scriptPath), "%s", kScriptPath);
    CHECK(g_world->add_script_component(scripted, sc), "attach script");

    CHECK(ticking_frame(pipeline), "run B frame 1");
    CHECK(game_state_is("clocks_zero"),
          "run B on_begin_play reads zeroed clocks, not run A's");

    pipeline.teardown();
  }

  engine::shutdown();
  remove_script_file();

  if (g_failures != 0) {
    std::fprintf(stderr, "pipeline_clock_reset_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("pipeline_clock_reset_test: all checks passed\n");
  return 0;
}
