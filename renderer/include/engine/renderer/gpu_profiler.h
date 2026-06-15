// Declares gpu profiler types and APIs for the Engine renderer system.

#pragma once

#include <cstdint>

namespace engine::renderer {

/// Enumerates gpu pass id values used by the engine.
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
  SpotShadowMap = 9U,
  PointShadowMap = 10U,
  Count = 11U,
};

/// Stores gpu profiler debug stats data used by the engine.
struct GpuProfilerDebugStats final {
  std::uint64_t beginMarksScene = 0U;
  std::uint64_t endMarksScene = 0U;
  std::uint64_t beginMarksTonemap = 0U;
  std::uint64_t endMarksTonemap = 0U;
  std::uint64_t beginMarksGBuffer = 0U;
  std::uint64_t endMarksGBuffer = 0U;
};

/// Initializes the owning system for gpu profiler.
bool initialize_gpu_profiler() noexcept;
/// Shuts down the owning system for gpu profiler.
void shutdown_gpu_profiler() noexcept;
/// Handles gpu profiler begin frame.
void gpu_profiler_begin_frame() noexcept;
/// Handles gpu profiler begin pass.
void gpu_profiler_begin_pass(GpuPassId pass) noexcept;
/// Handles gpu profiler end pass.
void gpu_profiler_end_pass(GpuPassId pass) noexcept;
/// Handles gpu profiler pass ms.
float gpu_profiler_pass_ms(GpuPassId pass) noexcept;
/// Handles gpu profiler debug stats.
GpuProfilerDebugStats gpu_profiler_debug_stats() noexcept;

} // namespace engine::renderer
