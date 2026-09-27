// Verifies engine smoke behavior for the Engine test suite.

#include "engine/engine.h"

#include "../asset_root.h"
#include "engine/core/cvar.h"
#include "engine/core/engine_stats.h"
#include "engine/renderer/gpu_profiler.h"

#include <filesystem>

namespace {

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
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

  const engine::RunResult runResult = engine::run(20U);
  if ((runResult != engine::RunResult::Stopped) ||
      (engine::run_result_exit_code(runResult) != 0)) {
    engine::shutdown();
    return 10;
  }

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
