#include "engine/renderer/gpu_profiler.h"

int main() {
  using namespace engine::renderer;

  // Without a GL context / render device, initialization should degrade
  // gracefully and leave timings at 0.
  if (!initialize_gpu_profiler()) {
    return 1;
  }

  gpu_profiler_begin_frame();
  gpu_profiler_begin_pass(GpuPassId::Scene);
  gpu_profiler_end_pass(GpuPassId::Scene);
  gpu_profiler_begin_pass(GpuPassId::Tonemap);
  gpu_profiler_end_pass(GpuPassId::Tonemap);

  const float sceneMs = gpu_profiler_pass_ms(GpuPassId::Scene);
  const float tonemapMs = gpu_profiler_pass_ms(GpuPassId::Tonemap);
  if ((sceneMs < 0.0F) || (tonemapMs < 0.0F)) {
    shutdown_gpu_profiler();
    return 2;
  }

  const GpuProfilerDebugStats debugStats = gpu_profiler_debug_stats();
  if ((debugStats.beginMarksScene == 0U) || (debugStats.endMarksScene == 0U) ||
      (debugStats.beginMarksTonemap == 0U) ||
      (debugStats.endMarksTonemap == 0U)) {
    shutdown_gpu_profiler();
    return 3;
  }

  shutdown_gpu_profiler();
  return 0;
}
