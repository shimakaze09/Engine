// Verifies the bundled scene controller builds its demo scene in player
// mode: engine::bootstrap in player mode, headless, with the default main
// script (assets/main.lua) on the bootstrap scene's controller entity and
// no startup scene to replace it. Its on_begin_play spawns the Player cube,
// three props and a ball; every named entity must exist after the first
// frames, and the script must not report a rolled-back setup.

#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {

int g_failures = 0;
int g_rollbacks = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Counts the scene controller's rollback report.
void count_rollbacks(engine::core::LogLevel, const char *, const char *message,
                     void *) noexcept {
  if ((message != nullptr) &&
      (std::strstr(message, "scene setup failed") != nullptr)) {
    ++g_rollbacks;
  }
}

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
        std::filesystem::exists(
            normalized / "assets/shaders/bgfx/shaders.manifest", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }
  return false;
}

void set_player_env() noexcept {
#ifdef _WIN32
  static_cast<void>(_putenv_s("ENGINE_CVAR_app_player_mode", "1"));
#else
  static_cast<void>(setenv("ENGINE_CVAR_app_player_mode", "1", 1));
#endif
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  set_player_env();

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  // No startup scene: player mode would otherwise replace the bootstrap
  // scene, controller and all, on the first frames.
  config.editorScenePath = "";
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    return 2;
  }
  const bool sinkOk =
      engine::core::log_register_sink(&count_rollbacks, nullptr);
  CHECK(sinkOk, "register the log sink");

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      pipeline.teardown();
      engine::shutdown();
      return 3;
    }
    // Player mode plays from the first frame; the scene controller begins
    // play on it and the Player's own script in a later pass.
    for (int frame = 0; frame < 3; ++frame) {
      CHECK(pipeline.execute_frame(), "frame");
    }
    const engine::runtime::World *world = pipeline.world();
    CHECK(world != nullptr, "pipeline world available");
    if (world != nullptr) {
      const char *names[] = {"Player", "Sphere Prop", "Cylinder Prop",
                             "Pyramid Prop", "Ball"};
      for (const char *name : names) {
        if (world->find_entity_by_name(name) ==
            engine::runtime::kInvalidEntity) {
          std::fprintf(stderr, "FAIL: main.lua did not spawn '%s'\n", name);
          ++g_failures;
        }
      }
    }
    CHECK(g_rollbacks == 0, "the scene controller's setup did not roll back");
    pipeline.teardown();
  }

  if (sinkOk) {
    engine::core::log_unregister_sink(&count_rollbacks, nullptr);
  }
  engine::shutdown();
  if (g_failures != 0) {
    std::fprintf(stderr, "main_script_setup_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("main_script_setup_test: all checks passed\n");
  return 0;
}
