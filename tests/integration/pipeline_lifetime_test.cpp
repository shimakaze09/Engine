// Regression for #340: an EnginePipeline closes its run on every exit from
// that run, not only on an explicit teardown() call. The pipeline publishes
// aliases into Impl-owned storage while a run is open (the editor bridge's
// world, the scripting runtime binding's service locator, the editor asset
// service), so a run that ends without teardown leaves those aliases naming
// released storage and leaves run-scoped state dirty for the next run.
//
// Drives real engine::bootstrap() plus real EnginePipeline runs through the
// production entry points (the player_mode_test.cpp headless pattern) and
// covers the two exits that bypassed teardown: destruction of a pipeline
// whose run is still open, and initialize() called on a pipeline that
// already holds one.

#include "engine/core/input.h"
#include "engine/engine.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"
#include "engine/scripting/bindable_api.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

engine::runtime::World *g_world = nullptr;

/// Mirrors the pipeline's published world alias so the test can observe when
/// a run publishes and clears it.
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }

bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as player_mode_test.cpp).
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

/// Dirties the run-scoped state teardown is responsible for clearing, using
/// the same entry points gameplay scripts reach it through.
void dirty_run_state() noexcept {
  CHECK(engine::core::register_action("pipeline_lifetime_probe_action", 44),
        "register probe action");
  CHECK(engine::core::register_axis("pipeline_lifetime_probe_axis", 4, 7),
        "register probe axis");
  CHECK(engine::scripting::bindable_set_game_state("in_progress"),
        "move the game state off its default");

  CHECK(engine::core::gameplay_action_count() > 0U, "probe action is live");
  CHECK(engine::core::gameplay_axis_count() > 0U, "probe axis is live");
}

/// Compares the live game-state label against an expected value.
bool game_state_is(const char *expected) noexcept {
  const char *actual = engine::scripting::bindable_get_game_state();
  return (actual != nullptr) && (std::strcmp(actual, expected) == 0);
}

/// Reports one closed-run observable, naming both the exit under test and
/// the observable, since the same set is checked after several exits.
void check_closed(bool condition, const char *exit, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s: %s\n", exit, what);
    ++g_failures;
  }
}

/// Asserts every observable a closed run must leave behind.
void check_run_closed(const char *exit) noexcept {
  check_closed(g_world == nullptr, exit, "the published world alias is clear");
  check_closed(engine::core::gameplay_action_count() == 0U, exit,
               "the run's input actions are released");
  check_closed(engine::core::gameplay_axis_count() == 0U, exit,
               "the run's input axes are released");
  check_closed(game_state_is("startup"), exit,
               "the game state is back to its default");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  // Headless bootstrap keeps the null render device standing in, so the run
  // opens on every CI lane.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }

  // --- Destroying a pipeline mid-run closes that run. ---
  {
    {
      engine::EnginePipeline pipeline;
      if (!pipeline.initialize(0U)) {
        std::fprintf(stderr, "FAIL: pipeline A initialize\n");
        pipeline.teardown();
        engine::shutdown();
        return 3;
      }
      CHECK(g_world != nullptr, "run A published its world alias");
      CHECK(pipeline.execute_frame(), "run A frame");
      dirty_run_state();
      // The run is left open: the pipeline leaves scope without teardown().
    }

    check_run_closed("destruction closes an open run");
  }

  // --- initialize() on a pipeline that already holds a run closes it. ---
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline B initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 4;
    }
    CHECK(pipeline.execute_frame(), "run B frame");
    dirty_run_state();
    CHECK(pipeline.world() != nullptr, "run B holds a world");

    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline C initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 5;
    }

    // The replacement run is open, so its own alias is published; the
    // previous run's residue must be gone all the same.
    CHECK(engine::core::gameplay_action_count() == 0U,
          "replacing a run clears the previous run's input bindings");
    CHECK(engine::core::gameplay_axis_count() == 0U,
          "replacing a run clears the previous run's axes");
    CHECK(game_state_is("startup"),
          "replacing a run resets the previous run's game state");
    // Address identity says nothing here: closing the previous run frees its
    // World before the replacement allocates, so the allocator may hand back
    // the same address. What must hold is that the alias names the live run.
    CHECK(pipeline.world() != nullptr, "the replacement run holds a world");
    CHECK(g_world == pipeline.world(),
          "the replacement run published its own world alias");
    CHECK(pipeline.execute_frame(), "run C frame");

    pipeline.teardown();
    check_run_closed("teardown closes the replacement run");
  }

  engine::shutdown();
  engine::runtime::set_editor_bridge(nullptr);

  if (g_failures != 0) {
    std::fprintf(stderr, "pipeline_lifetime_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("pipeline_lifetime_test: all checks passed\n");
  return 0;
}
