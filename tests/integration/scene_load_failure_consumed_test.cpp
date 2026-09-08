// Regression test for #451: a scene load request that fails is attempted
// once and diagnosed once, not retried and re-logged on every active
// frame. Drives the production EnginePipeline headlessly (the
// scene_commit_camera_history_test.cpp bootstrap pattern) through the
// same script-style request path engine.load_scene and the player boot
// use, counts the pipeline's failure diagnostic through a log sink, and
// reads the World (content epoch, the authored entity) to prove the live
// scene survives every frame. It then proves an explicit new request
// still loads, and that a failing request never blocks a later valid one.

#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/math/vec3.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <thread>

namespace {

constexpr const char *kValidSceneFile = "slfc_valid.scene.json";
constexpr const char *kMissingSceneFile = "slfc_missing.scene.json";
constexpr const char *kMalformedSceneFile = "slfc_malformed.scene.json";
constexpr const char *kFailureMarker = "failed to process pending scene load";

engine::runtime::World *g_world = nullptr;
int g_failures = 0;

/// Counts the pipeline's scene-load failure diagnostics; the sink does
/// fixed-size work only, as the logging contract requires.
struct DiagnosticCounter final {
  int failureDiagnostics = 0;
};
DiagnosticCounter g_counter{};

void count_failure_diagnostics(engine::core::LogLevel level, const char *,
                               const char *message, void *userData) noexcept {
  auto *counter = static_cast<DiagnosticCounter *>(userData);
  if ((level == engine::core::LogLevel::Error) && (message != nullptr) &&
      (std::strstr(message, kFailureMarker) != nullptr)) {
    ++counter->failureDiagnostics;
  }
}

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return true; }
bool bridge_is_paused() noexcept { return false; }

/// Walks upward from the current path until the bundled assets are found.
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

bool write_text_file(const char *path, const char *contents) noexcept {
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
  const std::size_t length = std::strlen(contents);
  const bool ok = std::fwrite(contents, 1U, length, file) == length;
  std::fclose(file);
  return ok;
}

void cleanup_files() noexcept {
  static_cast<void>(std::remove(kValidSceneFile));
  static_cast<void>(std::remove(kMalformedSceneFile));
  static_cast<void>(std::remove(kMissingSceneFile));
}

/// One valid scene through the production serializer (one scene object)
/// and one file the parser refuses.
bool write_scene_fixtures() noexcept {
  std::unique_ptr<engine::runtime::World> world(
      new (std::nothrow) engine::runtime::World());
  if (world == nullptr) {
    return false;
  }
  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(3.0F, 0.0F, 0.0F);
  if (world->create_scene_object(transform) == engine::runtime::kInvalidEntity) {
    return false;
  }
  return engine::runtime::save_scene(*world, kValidSceneFile) &&
         write_text_file(kMalformedSceneFile, "{ \"version\": ");
}

/// Runs one playing frame guaranteed to simulate at least one fixed step
/// (see pipeline_tick_cadence_test.cpp on the wall-clock accumulator).
bool ticking_frame(engine::EnginePipeline &pipeline) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return pipeline.execute_frame();
}

/// Requests `path`, runs `frames` active frames, and checks that the
/// request failed on its first frame only: one diagnostic in total, the
/// request gone after the first frame, the World's epoch and authored
/// entity untouched throughout.
void expect_single_failed_attempt(engine::EnginePipeline &pipeline,
                                  const char *path, int frames,
                                  engine::runtime::Entity authored,
                                  const char *label) noexcept {
  const std::uint32_t epochBefore = g_world->content_epoch();
  const int diagnosticsBefore = g_counter.failureDiagnostics;
  CHECK(engine::scripting::request_scene_load(path), label);

  CHECK(ticking_frame(pipeline), "frame runs");
  CHECK(g_counter.failureDiagnostics == diagnosticsBefore + 1,
        "the failed attempt is diagnosed once on its frame");
  CHECK(!engine::scripting::has_pending_scene_op(),
        "the failed request is consumed by its one attempt");

  for (int frame = 1; frame < frames; ++frame) {
    CHECK(ticking_frame(pipeline), "frame runs");
  }
  CHECK(g_counter.failureDiagnostics == diagnosticsBefore + 1,
        "later frames neither retry nor re-log the failed request");
  CHECK(g_world->content_epoch() == epochBefore,
        "no commit happened on any frame");
  CHECK(g_world->is_alive(authored), "the live scene's entity survives");
}

} // namespace

/// Runs this executable or test program.
int main() {
  cleanup_files();
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!write_scene_fixtures()) {
    std::fprintf(stderr, "FAIL: write scene fixtures\n");
    cleanup_files();
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  // Full production bootstrap in headless mode: the null render device
  // stands in so the pipeline runs on every CI lane.
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    cleanup_files();
    return 2;
  }
  if (!engine::core::log_register_sink(&count_failure_diagnostics,
                                       &g_counter)) {
    std::fprintf(stderr, "FAIL: register log sink\n");
    engine::shutdown();
    cleanup_files();
    return 2;
  }

  engine::EnginePipeline pipeline;
  if (!pipeline.initialize(0U) || (g_world == nullptr)) {
    std::fprintf(stderr, "FAIL: pipeline initialize\n");
    pipeline.teardown();
    engine::core::log_unregister_sink(&count_failure_diagnostics, &g_counter);
    engine::shutdown();
    cleanup_files();
    return 3;
  }

  const engine::runtime::Entity authored = g_world->create_scene_object();
  CHECK(authored != engine::runtime::kInvalidEntity, "author a live entity");
  CHECK(ticking_frame(pipeline), "frame: baseline");
  CHECK(g_counter.failureDiagnostics == 0, "no diagnostic before any request");

  // --- Case 1: a missing file fails once across many frames. ---
  // Base behavior: the request stays queued, so every one of these frames
  // opens the path again and logs the same error again.
  expect_single_failed_attempt(pipeline, kMissingSceneFile, 6, authored,
                               "request missing scene");

  // --- Case 2: a malformed file is the same contract. ---
  expect_single_failed_attempt(pipeline, kMalformedSceneFile, 4, authored,
                               "request malformed scene");

  // --- Case 3: an explicit new request after a failure still loads, and
  // a second identical request for the missing file fails once again
  // (a retry is a new request, never an automatic one). ---
  {
    const std::uint32_t epochBefore = g_world->content_epoch();
    const int diagnosticsBefore = g_counter.failureDiagnostics;
    CHECK(engine::scripting::request_scene_load(kValidSceneFile),
          "request valid scene");
    CHECK(ticking_frame(pipeline), "frame: commit");
    CHECK(g_world->content_epoch() != epochBefore,
          "the valid request committed");
    CHECK(!engine::scripting::has_pending_scene_op(),
          "the committed request is cleared");
    CHECK(g_world->alive_entity_count() == 1U,
          "the replacement holds the fixture's one entity");
    CHECK(g_counter.failureDiagnostics == diagnosticsBefore,
          "a successful load logs no failure");

    const std::uint32_t committedEpoch = g_world->content_epoch();
    CHECK(engine::scripting::request_scene_load(kMissingSceneFile),
          "request missing scene again");
    CHECK(ticking_frame(pipeline), "frame: second failure");
    CHECK(ticking_frame(pipeline), "frame: after second failure");
    CHECK(g_counter.failureDiagnostics == diagnosticsBefore + 1,
          "the re-requested failure is diagnosed exactly once more");
    CHECK(g_world->content_epoch() == committedEpoch,
          "the replacement scene survives the second failure");
  }

  pipeline.teardown();
  engine::core::log_unregister_sink(&count_failure_diagnostics, &g_counter);
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  cleanup_files();

  if (g_failures != 0) {
    std::fprintf(stderr, "scene_load_failure_consumed_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("scene_load_failure_consumed_test: all checks passed\n");
  return 0;
}
