// A saved scene names its meshes by id alone. This test reopens the kit
// scene (assets/coin_run.scene) through the pipeline's scene
// operation, headless and out of play so no script runs, and requires
// every mesh component to reach Ready through the asset catalog, with
// more draw commands than the built-in and bootstrap meshes alone
// produce. It also requires the catalog to answer the editor's questions
// before anything loads: the Mesh picker lists the project's meshes and
// the built-in primitives, and a loaded mesh's id reads back as its path.
// A reference the catalog cannot place is reported exactly once, as a
// diagnostic record carrying the id and the entity.
//
// Readiness is observed through the pipeline's own slice diagnostics
// line, which reports mesh components and how many are Ready; the loop
// is bounded by a frame count, never by time.

#include "engine/core/diagnostic.h"
#include "engine/core/engine_stats.h"
#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

constexpr const char *kScene = "assets/coin_run.scene";
constexpr const char *kMainScript = "scene_mesh_resolution_main.lua";
constexpr const char *kCoinPath = "assets/props/coin.mesh";
constexpr const char *kCubePath = "builtin://cube";
constexpr double kStepSeconds = 1.0 / 60.0;
/// The slice diagnostics line comes every 60 frames; this bounds a run
/// that never becomes ready, it is not a duration.
constexpr int kMaxFrames = 6000;
/// An id no asset carries.
constexpr std::uint64_t kUnknownMeshId = 0x0BAD0BAD0BAD0BADULL;

engine::runtime::World *g_world = nullptr;
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

std::size_t g_meshComponents = 0U;
std::size_t g_readyMeshComponents = 0U;
int g_unknownReports = 0;
std::uint32_t g_unknownReportEntity = 0U;

/// Reads the two mesh counts from the pipeline's slice diagnostics line.
void watch_log(engine::core::LogLevel, const char *, const char *message,
               void *) noexcept {
  if (message == nullptr) {
    return;
  }
  const char *meshes = std::strstr(message, "meshComponents=");
  const char *ready = std::strstr(message, "readyMeshComponents=");
  if ((meshes == nullptr) || (ready == nullptr)) {
    return;
  }
  g_meshComponents = static_cast<std::size_t>(
      std::strtoull(meshes + std::strlen("meshComponents="), nullptr, 10));
  g_readyMeshComponents = static_cast<std::size_t>(std::strtoull(
      ready + std::strlen("readyMeshComponents="), nullptr, 10));
}

void watch_diagnostics(const engine::core::Diagnostic &record,
                       void *) noexcept {
  if (record.assetId == kUnknownMeshId) {
    ++g_unknownReports;
    g_unknownReportEntity = record.entityPersistentId;
  }
}

bool set_working_directory_with_assets() noexcept {
  std::error_code ec{};
  const std::filesystem::path original = std::filesystem::current_path(ec);
  if (ec) {
    return false;
  }
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};
  for (const std::filesystem::path &candidate : candidates) {
    ec.clear();
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec || !std::filesystem::exists(normalized / kScene, ec)) {
      continue;
    }
    std::filesystem::current_path(normalized, ec);
    return !ec;
  }
  return false;
}

bool write_empty_main_script() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kMainScript, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kMainScript, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const char text[] = "-- Empty on purpose: nothing may request a mesh.\n";
  const bool ok = std::fwrite(text, 1U, sizeof(text) - 1U, file) ==
                  (sizeof(text) - 1U);
  return (std::fclose(file) == 0) && ok;
}

void remove_files() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kMainScript, ec);
}

bool frame(engine::EnginePipeline &pipeline) noexcept {
  return pipeline.set_frame_delta_override(kStepSeconds) &&
         pipeline.execute_frame();
}

/// True when the Mesh query lists `path`.
bool query_lists(const char *path) noexcept {
  constexpr std::size_t kMaxHits = 128U;
  engine::runtime::EditorAssetSearchResult hits[kMaxHits];
  const std::size_t count = engine::runtime::editor_query_assets(
      engine::content::AssetTypeTag::Mesh, "", hits, kMaxHits);
  for (std::size_t i = 0U; i < count; ++i) {
    if (std::strcmp(hits[i].path, path) == 0) {
      return true;
    }
  }
  return false;
}

int g_failures = 0;
void check(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

/// Runs frames until the slice diagnostics report every mesh component
/// Ready, or the frame bound is spent. Returns the frames run.
int pump_until_ready(engine::EnginePipeline &pipeline) noexcept {
  int frames = 0;
  while (frames < kMaxFrames) {
    if (!frame(pipeline)) {
      return -1;
    }
    ++frames;
    if ((g_meshComponents > 0U) &&
        (g_readyMeshComponents == g_meshComponents)) {
      return frames;
    }
  }
  return frames;
}

void run(engine::EnginePipeline &pipeline) noexcept {
  const std::uint64_t coinId = engine::renderer::make_asset_id_from_path(kCoinPath);
  const std::uint64_t cubeId = engine::renderer::make_asset_id_from_path(kCubePath);

  // The catalog answers before any scene asks: rows 1 and 2 of the issue.
  check(query_lists(kCoinPath), "the Mesh query lists the kit's coin mesh");
  check(query_lists(kCubePath), "the Mesh query lists the built-in cube");
  char path[260] = {};
  check(engine::runtime::editor_asset_display_path(coinId, path, sizeof(path)) &&
            (std::strcmp(path, kCoinPath) == 0),
        "the coin mesh id reads back as its path");
  check(engine::runtime::editor_asset_display_path(cubeId, path, sizeof(path)) &&
            (std::strcmp(path, kCubePath) == 0),
        "the built-in cube id reads back as its path");

  // Reopen the saved scene out of play: no script can name a mesh.
  check(engine::scripting::request_scene_load(kScene) && frame(pipeline),
        "the scene load request is applied");
  check(g_world->find_entity_by_name("Coin1") != engine::runtime::kInvalidEntity,
        "the reopened scene holds its coins");
  // The load commits after the frame's render, so the first draw count
  // of the reopened scene is published by the frame after it.
  check(frame(pipeline), "the first frame of the reopened scene ran");
  const std::uint32_t drawsBefore = engine::core::get_engine_stats().drawCommands;

  const int frames = pump_until_ready(pipeline);
  std::printf("scene_mesh_resolution_test: %zu of %zu mesh components ready "
              "after %d frame(s); draws %u before, %u after\n",
              g_readyMeshComponents, g_meshComponents, frames, drawsBefore,
              engine::core::get_engine_stats().drawCommands);
  check(frames > 0, "frames ran");
  check(g_meshComponents >= 14U,
        "the scene carries its fourteen mesh components");
  check((g_meshComponents > 0U) && (g_readyMeshComponents == g_meshComponents),
        "every mesh component's asset reaches Ready with no script running");
  check(engine::core::get_engine_stats().drawCommands > drawsBefore,
        "the resolved meshes add draw commands");

  // A reference nothing can place: reported once, with id and entity,
  // and a second entity carrying the same id adds no report.
  const engine::runtime::Entity orphan = g_world->create_scene_object();
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = kUnknownMeshId;
  check((orphan != engine::runtime::kInvalidEntity) &&
            g_world->add_mesh_component(orphan, mesh),
        "an entity with an unplaceable mesh id is added");
  const engine::runtime::Entity twin = g_world->create_scene_object();
  check((twin != engine::runtime::kInvalidEntity) &&
            g_world->add_mesh_component(twin, mesh),
        "a second entity with the same unplaceable id is added");
  for (int i = 0; i < 120; ++i) {
    if (!frame(pipeline)) {
      check(false, "frames after the unplaceable reference ran");
      break;
    }
  }
  check(g_unknownReports == 1, "an unplaceable mesh id is reported exactly once");
  check(g_unknownReportEntity == g_world->persistent_id(orphan),
        "the report names the first entity carrying the id");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate %s\n", kScene);
    return 1;
  }
  if (!write_empty_main_script()) {
    std::fprintf(stderr, "FAIL: could not write the main script\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.mainScriptPath = kMainScript;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap\n");
    remove_files();
    return 2;
  }
  const bool sinksRegistered =
      engine::core::log_register_sink(&watch_log, nullptr) &&
      engine::core::log_register_diagnostic_sink(&watch_diagnostics, nullptr);

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      g_failures = 1;
    } else {
      check(sinksRegistered, "the log sinks registered");
      run(pipeline);
    }
    pipeline.teardown();
  }

  engine::core::log_unregister_diagnostic_sink(&watch_diagnostics, nullptr);
  engine::core::log_unregister_sink(&watch_log, nullptr);
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  remove_files();

  if (g_failures != 0) {
    return 10;
  }
  std::printf("scene_mesh_resolution_test: all checks passed\n");
  return 0;
}
