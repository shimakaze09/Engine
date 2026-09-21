// A saved scene names its materials by persistent identity, and the
// shading model a surface draws with lives on the material it names. This
// test opens assets/shading_models.scene through the pipeline's scene
// operation, headless and out of play so no script binds anything, and
// requires each mesh component's authored material reference to resolve to
// the material the document names, with the shading model that material
// authored.
//
// It also saves the loaded world back out and reloads it, because the
// binding is only durable if it survives a round trip: an entity that
// resolves in-session but writes no reference loses its material silently
// the next time the scene opens (#634).
//
// Readiness is observed through the pipeline's own slice diagnostics line;
// the loop is bounded by a frame count, never by time.

#include "engine/core/logging.h"
#include "engine/engine.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"
#include "engine/scripting/scripting.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

constexpr const char *kScene = "assets/shading_models.scene";
constexpr const char *kMainScript = "scene_material_resolution_main.lua";
constexpr const char *kSavedScene = "scene_material_resolution_saved.scene";
constexpr double kStepSeconds = 1.0 / 60.0;
/// The slice diagnostics line comes every 60 frames; this bounds a run
/// that never becomes ready, it is not a duration.
constexpr int kMaxFrames = 6000;

/// One authored surface: the entity the scene names, the material it
/// references, and the shading model that material file declares.
struct Subject final {
  const char *entity;
  const char *materialPath;
  engine::renderer::ShadingModel model;
};

constexpr Subject kSubjects[] = {
    {"PbrSphere", "assets/materials/pbr_example.mat",
     engine::renderer::ShadingModel::Pbr},
    {"ToonSphere", "assets/materials/toon_example.mat",
     engine::renderer::ShadingModel::Toon},
    {"UnlitSphere", "assets/materials/unlit_example.mat",
     engine::renderer::ShadingModel::Unlit},
};
constexpr std::size_t kSubjectCount =
    sizeof(kSubjects) / sizeof(kSubjects[0]);

engine::runtime::World *g_world = nullptr;
void capture_world(engine::runtime::World *world) noexcept { g_world = world; }
bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

std::size_t g_meshComponents = 0U;
std::size_t g_readyMeshComponents = 0U;

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
  const char text[] = "-- Empty on purpose: nothing may bind a material.\n";
  const bool ok = std::fwrite(text, 1U, sizeof(text) - 1U, file) ==
                  (sizeof(text) - 1U);
  return (std::fclose(file) == 0) && ok;
}

void remove_files() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kMainScript, ec);
  ec.clear();
  std::filesystem::remove(kSavedScene, ec);
}

bool frame(engine::EnginePipeline &pipeline) noexcept {
  return pipeline.set_frame_delta_override(kStepSeconds) &&
         pipeline.execute_frame();
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

/// The material id each subject's mesh component resolved to, or zero.
void collect_bound_ids(std::uint64_t *outIds) noexcept {
  for (std::size_t i = 0U; i < kSubjectCount; ++i) {
    outIds[i] = 0ULL;
    const engine::runtime::Entity entity =
        g_world->find_entity_by_name(kSubjects[i].entity);
    if (entity == engine::runtime::kInvalidEntity) {
      continue;
    }
    engine::math::MeshComponent mesh{};
    if (!g_world->get_mesh_component(entity, &mesh)) {
      continue;
    }
    outIds[i] = mesh.materialAssetId;
  }
}

void run(engine::EnginePipeline &pipeline) noexcept {
  // The catalog must keep the identity each material's sidecar authored.
  // A material record registered without it cannot be found by reference,
  // so no document could name it and the picker would store nothing.
  for (const Subject &subject : kSubjects) {
    const engine::runtime::EditorMaterialState state =
        engine::runtime::editor_load_material(subject.materialPath);
    check(state.found, "the material asset loads");
    check(state.params.shadingModel == subject.model,
          "the material file's shading model is the one it authored");
    check(engine::core::asset_ref_is_valid(
              engine::runtime::editor_asset_ref(state.materialId)),
          "the catalogued material keeps its authored identity");
  }

  // Open the authored scene out of play: no script can bind a material.
  check(engine::scripting::request_scene_load(kScene) && frame(pipeline),
        "the scene load request is applied");
  const int frames = pump_until_ready(pipeline);
  check(frames > 0, "frames ran");
  check(g_meshComponents >= kSubjectCount,
        "the scene carries one mesh component per shading model");
  check((g_meshComponents > 0U) && (g_readyMeshComponents == g_meshComponents),
        "every mesh component's asset reaches Ready with no script running");

  std::uint64_t bound[kSubjectCount] = {};
  collect_bound_ids(bound);
  for (std::size_t i = 0U; i < kSubjectCount; ++i) {
    const engine::runtime::EditorMaterialState state =
        engine::runtime::editor_load_material(kSubjects[i].materialPath);
    std::printf("scene_material_resolution_test: %s bound %llu, expected "
                "%llu\n",
                kSubjects[i].entity,
                static_cast<unsigned long long>(bound[i]),
                static_cast<unsigned long long>(state.materialId));
    check(bound[i] != 0ULL,
          "the authored material reference resolved to a material");
    check(bound[i] == state.materialId,
          "it resolved to the material the document names");
  }
  // Three distinct materials, so a single shared fallback cannot pass.
  check((bound[0] != bound[1]) && (bound[1] != bound[2]) &&
            (bound[0] != bound[2]),
        "the three surfaces bound three different materials");

  // Durability: saving the loaded world and reopening it must keep every
  // binding. A reference that resolves in-session but is not written back
  // loses the material the next time the scene opens.
  check(engine::runtime::save_scene(*g_world, kSavedScene),
        "the loaded world saves");
  check(engine::scripting::request_scene_load(kSavedScene) && frame(pipeline),
        "the saved scene load request is applied");
  const int reloadFrames = pump_until_ready(pipeline);
  check(reloadFrames > 0, "frames ran after the reload");

  std::uint64_t reloaded[kSubjectCount] = {};
  collect_bound_ids(reloaded);
  for (std::size_t i = 0U; i < kSubjectCount; ++i) {
    check(reloaded[i] == bound[i],
          "the material binding survives a save and reload");
  }
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
  const bool sinkRegistered =
      engine::core::log_register_sink(&watch_log, nullptr);

  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U) || (g_world == nullptr)) {
      std::fprintf(stderr, "FAIL: pipeline initialize\n");
      g_failures = 1;
    } else {
      check(sinkRegistered, "the log sink registered");
      run(pipeline);
    }
    pipeline.teardown();
  }

  engine::core::log_unregister_sink(&watch_log, nullptr);
  engine::runtime::set_editor_bridge(nullptr);
  engine::shutdown();
  remove_files();

  if (g_failures != 0) {
    return 10;
  }
  std::printf("scene_material_resolution_test: all checks passed\n");
  return 0;
}
