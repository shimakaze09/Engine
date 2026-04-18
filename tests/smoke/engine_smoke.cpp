#include "engine/engine.h"

#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/renderer/gpu_profiler.h"

#include <filesystem>

namespace {

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

} // namespace

int main() {
  if (!set_working_directory_with_assets()) {
    return 1;
  }

  if (!engine::bootstrap()) {
    return 2;
  }

  if (!engine::core::cvar_set_bool("r_showStats", false)) {
    engine::shutdown();
    return 3;
  }

  if (engine::core::cvar_get_bool("r_showStats", true)) {
    engine::shutdown();
    return 4;
  }

  if (!engine::core::cvar_set_bool("r_showStats", true)) {
    engine::shutdown();
    return 5;
  }

  engine::run(20U);

  const engine::core::EngineStats stats = engine::core::get_engine_stats();
  if ((stats.frameTimeMs <= 0.0F) || (stats.fps <= 0.0F)) {
    engine::shutdown();
    return 6;
  }

  if (stats.entityCount == 0U) {
    engine::shutdown();
    return 7;
  }

  if ((stats.gpuSceneMs < 0.0F) || (stats.gpuTonemapMs < 0.0F)) {
    engine::shutdown();
    return 8;
  }

  const engine::renderer::GpuProfilerDebugStats gpuDebug =
      engine::renderer::gpu_profiler_debug_stats();
  // Accept either forward (Scene) or deferred (GBuffer) geometry pass.
  const bool sceneOk =
      (gpuDebug.beginMarksScene > 0U) && (gpuDebug.endMarksScene > 0U);
  const bool gbufferOk =
      (gpuDebug.beginMarksGBuffer > 0U) && (gpuDebug.endMarksGBuffer > 0U);
  if (!sceneOk && !gbufferOk) {
    engine::shutdown();
    return 9;
  }
  if ((gpuDebug.beginMarksTonemap == 0U) || (gpuDebug.endMarksTonemap == 0U)) {
    engine::shutdown();
    return 9;
  }

  engine::shutdown();
  return 0;
}
