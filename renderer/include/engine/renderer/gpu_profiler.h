#pragma once

#include <cstdint>

namespace engine::renderer {

enum class GpuPassId : std::uint8_t {
  Scene = 0U,
  Tonemap = 1U,
  Count = 2U,
};

struct GpuProfilerDebugStats final {
  std::uint64_t beginMarksScene = 0U;
  std::uint64_t endMarksScene = 0U;
  std::uint64_t beginMarksTonemap = 0U;
  std::uint64_t endMarksTonemap = 0U;
};

bool initialize_gpu_profiler() noexcept;
void shutdown_gpu_profiler() noexcept;
void gpu_profiler_begin_frame() noexcept;
void gpu_profiler_begin_pass(GpuPassId pass) noexcept;
void gpu_profiler_end_pass(GpuPassId pass) noexcept;
float gpu_profiler_pass_ms(GpuPassId pass) noexcept;
GpuProfilerDebugStats gpu_profiler_debug_stats() noexcept;

} // namespace engine::renderer
