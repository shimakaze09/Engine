#include "engine/renderer/gpu_profiler.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/logging.h"
#include "engine/renderer/render_device.h"

namespace engine::renderer {

namespace {

constexpr std::size_t kPassCount = static_cast<std::size_t>(GpuPassId::Count);
constexpr std::size_t kFrameLag = 2U;

struct QueryRange final {
  std::uint32_t beginQuery = 0U;
  std::uint32_t endQuery = 0U;
  bool submitted = false;
};

struct GpuProfilerState final {
  bool initialized = false;
  bool supported = false;
  std::size_t writeFrame = 0U;
  std::size_t readFrame = 0U;
  std::array<std::array<QueryRange, kPassCount>, kFrameLag> queryFrames{};
  std::array<float, kPassCount> passDurationsMs{};
  GpuProfilerDebugStats debugStats{};
};

GpuProfilerState g_gpuProfiler{};

std::size_t pass_index(GpuPassId pass) noexcept {
  const std::size_t idx = static_cast<std::size_t>(pass);
  return (idx < kPassCount) ? idx : 0U;
}

bool query_range_ready(const RenderDevice *dev,
                       const QueryRange &range) noexcept {
  if ((dev == nullptr) || !range.submitted ||
      (dev->query_result_available == nullptr)) {
    return false;
  }

  return dev->query_result_available(range.beginQuery) &&
         dev->query_result_available(range.endQuery);
}

void resolve_read_frame(const RenderDevice *dev) noexcept {
  if ((dev == nullptr) || (dev->query_result_u64 == nullptr)) {
    return;
  }

  auto &frameRanges = g_gpuProfiler.queryFrames[g_gpuProfiler.readFrame];
  for (std::size_t i = 0U; i < kPassCount; ++i) {
    QueryRange &range = frameRanges[i];
    if (!query_range_ready(dev, range)) {
      continue;
    }

    const std::uint64_t beginNs = dev->query_result_u64(range.beginQuery);
    const std::uint64_t endNs = dev->query_result_u64(range.endQuery);
    g_gpuProfiler.passDurationsMs[i] =
        (endNs > beginNs)
            ? static_cast<float>(static_cast<double>(endNs - beginNs) /
                                 1000000.0)
            : 0.0F;
    range.submitted = false;
  }
}

} // namespace

bool initialize_gpu_profiler() noexcept {
  if (g_gpuProfiler.initialized) {
    return true;
  }

  g_gpuProfiler = GpuProfilerState{};

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->create_query == nullptr) ||
      (dev->destroy_query == nullptr) ||
      (dev->query_counter_timestamp == nullptr) ||
      (dev->query_result_available == nullptr) ||
      (dev->query_result_u64 == nullptr)) {
    core::log_message(
        core::LogLevel::Warning, "renderer",
        "GPU profiler unavailable: timestamp query support missing");
    g_gpuProfiler.initialized = true;
    g_gpuProfiler.supported = false;
    return true;
  }

  for (std::size_t frame = 0U; frame < kFrameLag; ++frame) {
    for (std::size_t pass = 0U; pass < kPassCount; ++pass) {
      QueryRange &range = g_gpuProfiler.queryFrames[frame][pass];
      range.beginQuery = dev->create_query();
      range.endQuery = dev->create_query();
      if ((range.beginQuery == 0U) || (range.endQuery == 0U)) {
        core::log_message(
            core::LogLevel::Warning, "renderer",
            "GPU profiler disabled: failed to allocate query objects");
        shutdown_gpu_profiler();
        g_gpuProfiler.initialized = true;
        g_gpuProfiler.supported = false;
        return true;
      }
    }
  }

  g_gpuProfiler.initialized = true;
  g_gpuProfiler.supported = true;
  return true;
}

void shutdown_gpu_profiler() noexcept {
  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (dev->destroy_query != nullptr)) {
    for (auto &frameRanges : g_gpuProfiler.queryFrames) {
      for (QueryRange &range : frameRanges) {
        if (range.beginQuery != 0U) {
          dev->destroy_query(range.beginQuery);
        }
        if (range.endQuery != 0U) {
          dev->destroy_query(range.endQuery);
        }
        range = QueryRange{};
      }
    }
  }

  g_gpuProfiler = GpuProfilerState{};
}

void gpu_profiler_begin_frame() noexcept {
  if (!g_gpuProfiler.initialized) {
    static_cast<void>(initialize_gpu_profiler());
  }

  if (!g_gpuProfiler.supported) {
    return;
  }

  const RenderDevice *dev = render_device();
  resolve_read_frame(dev);

  g_gpuProfiler.writeFrame = (g_gpuProfiler.writeFrame + 1U) % kFrameLag;
  g_gpuProfiler.readFrame = (g_gpuProfiler.writeFrame + 1U) % kFrameLag;

  auto &writeRanges = g_gpuProfiler.queryFrames[g_gpuProfiler.writeFrame];
  for (QueryRange &range : writeRanges) {
    range.submitted = false;
  }
}

void gpu_profiler_begin_pass(GpuPassId pass) noexcept {
  if (pass == GpuPassId::Scene) {
    ++g_gpuProfiler.debugStats.beginMarksScene;
  } else if (pass == GpuPassId::Tonemap) {
    ++g_gpuProfiler.debugStats.beginMarksTonemap;
  }

  if (!g_gpuProfiler.supported) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->query_counter_timestamp == nullptr)) {
    return;
  }

  QueryRange &range =
      g_gpuProfiler.queryFrames[g_gpuProfiler.writeFrame][pass_index(pass)];
  dev->query_counter_timestamp(range.beginQuery);
}

void gpu_profiler_end_pass(GpuPassId pass) noexcept {
  if (pass == GpuPassId::Scene) {
    ++g_gpuProfiler.debugStats.endMarksScene;
  } else if (pass == GpuPassId::Tonemap) {
    ++g_gpuProfiler.debugStats.endMarksTonemap;
  }

  if (!g_gpuProfiler.supported) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->query_counter_timestamp == nullptr)) {
    return;
  }

  QueryRange &range =
      g_gpuProfiler.queryFrames[g_gpuProfiler.writeFrame][pass_index(pass)];
  dev->query_counter_timestamp(range.endQuery);
  range.submitted = true;
}

float gpu_profiler_pass_ms(GpuPassId pass) noexcept {
  return g_gpuProfiler.passDurationsMs[pass_index(pass)];
}

GpuProfilerDebugStats gpu_profiler_debug_stats() noexcept {
  return g_gpuProfiler.debugStats;
}

} // namespace engine::renderer
