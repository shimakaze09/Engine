#pragma once

#include <cstdint>

namespace engine::renderer {

enum class GpuPassId : std::uint8_t {
  Scene = 0U,
  Tonemap = 1U,
  GBuffer = 2U,
  DeferredLighting = 3U,
  GBufferDebug = 4U,
  Bloom = 5U,
  SSAO = 6U,
  ShadowMap = 7U,
  AutoExposure = 8U,
  Count = 9U,
};

struct GpuProfilerDebugStats final {
  std::uint64_t beginMarksScene = 0U;
  std::uint64_t endMarksScene = 0U;
  std::uint64_t beginMarksTonemap = 0U;
  std::uint64_t endMarksTonemap = 0U;
  std::uint64_t beginMarksGBuffer = 0U;
  std::uint64_t endMarksGBuffer = 0U;
};

bool initialize_gpu_profiler() noexcept;
void shutdown_gpu_profiler() noexcept;
void gpu_profiler_begin_frame() noexcept;
void gpu_profiler_begin_pass(GpuPassId pass) noexcept;
void gpu_profiler_end_pass(GpuPassId pass) noexcept;
float gpu_profiler_pass_ms(GpuPassId pass) noexcept;
GpuProfilerDebugStats gpu_profiler_debug_stats() noexcept;

} // namespace engine::renderer
