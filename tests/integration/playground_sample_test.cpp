// Verifies the playground sample plays as shipped: player mode boots
// assets/samples/playground.scene headless at a fixed 60 Hz frame delta.
// Every authored entity loads, the sample's controller spawns the Player,
// three props and the Ball without rolling back, and the two bodies it
// drops come to rest on the scene's ground rather than falling through
// it. The sample no longer rides on the engine's startup scene (now the
// empty template), so its floor must be its own.

#include "../asset_root.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <cmath>
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

void set_player_env() noexcept {
#ifdef _WIN32
  static_cast<void>(_putenv_s("ENGINE_CVAR_app_player_mode", "1"));
#else
  static_cast<void>(setenv("ENGINE_CVAR_app_player_mode", "1", 1));
#endif
}

/// Checks `name` rests on the ground: its centre is `restHeight` above the
/// surface at y = 0, within physics_rest_test's bound on a settled body's
/// centre (0.01 m, the solver's resting penetration plus contact jitter).
void check_rests(const engine::runtime::World &world, const char *name,
                 float restHeight) noexcept {
  const engine::runtime::Entity entity = world.find_entity_by_name(name);
  engine::runtime::Transform transform{};
  if ((entity == engine::runtime::kInvalidEntity) ||
      !world.get_transform(entity, &transform)) {
    std::fprintf(stderr, "FAIL: '%s' has no transform\n", name);
    ++g_failures;
    return;
  }
  if (std::fabs(transform.position.y - restHeight) > 0.01F) {
    std::fprintf(stderr,
                 "FAIL: '%s' is at y = %.4f, not resting at %.2f on the "
                 "ground\n",
                 name, static_cast<double>(transform.position.y),
                 static_cast<double>(restHeight));
    ++g_failures;
  }
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  set_player_env();

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.editorScenePath = "assets/samples/playground.scene";
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
    CHECK(pipeline.set_frame_delta_override(1.0 / 60.0),
          "fix the frame delta");
    // Five simulated seconds: the Ball, dropped 1.5 m with restitution
    // 0.5, has stopped bouncing well inside two.
    for (int frame = 0; frame < 300; ++frame) {
      CHECK(pipeline.execute_frame(), "frame");
    }
    const engine::runtime::World *world = pipeline.world();
    CHECK(world != nullptr, "pipeline world available");
    if (world != nullptr) {
      const char *names[] = {
          "Main Camera",   "Sun Light",        "Red Cube",
          "Blue Cube",     "Ground",           "Foliage Patch",
          "Character",     "Scene Controller", "Player",
          "Sphere Prop",   "Cylinder Prop",    "Pyramid Prop",
          "Ball"};
      for (const char *name : names) {
        if (world->find_entity_by_name(name) ==
            engine::runtime::kInvalidEntity) {
          std::fprintf(stderr, "FAIL: the sample has no '%s'\n", name);
          ++g_failures;
        }
      }
      // The Player is a unit cube and the Ball a unit-diameter sphere.
      check_rests(*world, "Player", 0.5F);
      check_rests(*world, "Ball", 0.5F);
    }
    CHECK(g_rollbacks == 0, "the scene controller's setup did not roll back");
    pipeline.teardown();
  }

  if (sinkOk) {
    engine::core::log_unregister_sink(&count_rollbacks, nullptr);
  }
  engine::shutdown();
  if (g_failures != 0) {
    std::fprintf(stderr, "playground_sample_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("playground_sample_test: all checks passed\n");
  return 0;
}
