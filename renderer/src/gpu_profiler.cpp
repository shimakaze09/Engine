// Implements gpu profiler behavior for the Engine renderer system.

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
  DeviceQueryHandle beginQuery{};
  DeviceQueryHandle endQuery{};
  bool submitted = false;
  bool beginIssued = false;
};

/// One ring slot: the query ranges a frame wrote plus whether that frame's
/// absent passes still have to be published. The published duration of a
/// pass is the value measured in the most recent frame the ring resolved,
/// and a pass that issued no queries in that frame publishes zero; the
/// flag makes that zeroing happen exactly once per frame written into the
/// slot, so a slot re-read while its GPU results are still pending cannot
/// keep overwriting a newer frame's measurement with an older absence.
struct QueryFrame final {
  std::array<QueryRange, kPassCount> ranges{};
  bool absentPassesPending = false;
};

struct GpuProfilerState final {
  bool initialized = false;
  bool supported = false;
  bool writeBlocked = false;
  std::size_t writeFrame = 0U;
  std::size_t readFrame = 0U;
  std::array<QueryFrame, kFrameLag> queryFrames{};
  std::array<float, kPassCount> passDurationsMs{};
  GpuProfilerDebugStats debugStats{};
};

GpuProfilerState g_gpuProfiler{};

/// Maps a pass id to its slot; kPassCount marks an out-of-range id so
/// callers skip it instead of silently corrupting the Scene slot.
std::size_t pass_index(GpuPassId pass) noexcept {
  const std::size_t idx = static_cast<std::size_t>(pass);
  return (idx < kPassCount) ? idx : kPassCount;
}

bool query_range_ready(const RenderDevice *dev,
                       const QueryRange &range) noexcept {
  if ((dev == nullptr) || !range.submitted ||
      (dev->timestamp_ready == nullptr)) {
    return false;
  }

  return dev->timestamp_ready(range.beginQuery) &&
         dev->timestamp_ready(range.endQuery);
}

void resolve_read_frame(const RenderDevice *dev) noexcept {
  if ((dev == nullptr) || (dev->timestamp_value == nullptr)) {
    return;
  }

  QueryFrame &frame = g_gpuProfiler.queryFrames[g_gpuProfiler.readFrame];
  const bool publishAbsences = frame.absentPassesPending;
  frame.absentPassesPending = false;
  for (std::size_t i = 0U; i < kPassCount; ++i) {
    QueryRange &range = frame.ranges[i];
    if (!range.submitted) {
      // The pass did not run in this frame, so its published duration is
      // zero rather than whatever an earlier frame measured; a range that
      // is submitted but not yet ready keeps the last value until it lands.
      if (publishAbsences) {
        g_gpuProfiler.passDurationsMs[i] = 0.0F;
      }
      continue;
    }
    if (!query_range_ready(dev, range)) {
      continue;
    }

    const std::uint64_t beginNs = dev->timestamp_value(range.beginQuery);
    const std::uint64_t endNs = dev->timestamp_value(range.endQuery);
    g_gpuProfiler.passDurationsMs[i] =
        (endNs > beginNs)
            ? static_cast<float>(static_cast<double>(endNs - beginNs) /
                                 1000000.0)
            : 0.0F;
    range.submitted = false;
  }
}

} // namespace

/// Initializes the owning system for gpu profiler.
bool initialize_gpu_profiler() noexcept {
  if (g_gpuProfiler.initialized) {
    return true;
  }

  g_gpuProfiler = GpuProfilerState{};

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || !dev->caps.timestampQueries ||
      (dev->create_timestamp_query == nullptr) ||
      (dev->destroy_timestamp_query == nullptr) ||
      (dev->write_timestamp == nullptr) ||
      (dev->timestamp_ready == nullptr) ||
      (dev->timestamp_value == nullptr)) {
    core::log_message(
        core::LogLevel::Warning, "renderer",
        "GPU profiler unavailable: timestamp query support missing");
    g_gpuProfiler.initialized = true;
    g_gpuProfiler.supported = false;
    return true;
  }

  for (std::size_t frame = 0U; frame < kFrameLag; ++frame) {
    for (std::size_t pass = 0U; pass < kPassCount; ++pass) {
      QueryRange &range = g_gpuProfiler.queryFrames[frame].ranges[pass];
      range.beginQuery = dev->create_timestamp_query();
      range.endQuery = dev->create_timestamp_query();
      if ((range.beginQuery == kInvalidDeviceQuery) ||
          (range.endQuery == kInvalidDeviceQuery)) {
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

/// Shuts down the owning system for gpu profiler.
void shutdown_gpu_profiler() noexcept {
  const RenderDevice *dev = render_device();
  if ((dev != nullptr) && (dev->destroy_timestamp_query != nullptr)) {
    for (QueryFrame &frame : g_gpuProfiler.queryFrames) {
      for (QueryRange &range : frame.ranges) {
        if (range.beginQuery != kInvalidDeviceQuery) {
          dev->destroy_timestamp_query(range.beginQuery);
        }
        if (range.endQuery != kInvalidDeviceQuery) {
          dev->destroy_timestamp_query(range.endQuery);
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

  QueryFrame &writeSlot = g_gpuProfiler.queryFrames[g_gpuProfiler.writeFrame];
  bool unresolved = false;
  for (const QueryRange &range : writeSlot.ranges) {
    unresolved = unresolved || range.submitted;
  }
  g_gpuProfiler.writeBlocked = unresolved;
  if (unresolved) {
    ++g_gpuProfiler.debugStats.droppedFrames;
    return;
  }
  for (QueryRange &range : writeSlot.ranges) {
    range.submitted = false;
    range.beginIssued = false;
  }
  writeSlot.absentPassesPending = true;
}

void gpu_profiler_begin_pass(GpuPassId pass) noexcept {
  if (pass == GpuPassId::Scene) {
    ++g_gpuProfiler.debugStats.beginMarksScene;
  } else if (pass == GpuPassId::Tonemap) {
    ++g_gpuProfiler.debugStats.beginMarksTonemap;
  } else if (pass == GpuPassId::GBuffer) {
    ++g_gpuProfiler.debugStats.beginMarksGBuffer;
  }

  if (!g_gpuProfiler.supported || g_gpuProfiler.writeBlocked) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->write_timestamp == nullptr)) {
    return;
  }

  const std::size_t idx = pass_index(pass);
  if (idx >= kPassCount) {
    return;
  }
  QueryRange &range =
      g_gpuProfiler.queryFrames[g_gpuProfiler.writeFrame].ranges[idx];
  dev->write_timestamp(range.beginQuery);
  range.beginIssued = true;
}

void gpu_profiler_end_pass(GpuPassId pass) noexcept {
  if (pass == GpuPassId::Scene) {
    ++g_gpuProfiler.debugStats.endMarksScene;
  } else if (pass == GpuPassId::Tonemap) {
    ++g_gpuProfiler.debugStats.endMarksTonemap;
  } else if (pass == GpuPassId::GBuffer) {
    ++g_gpuProfiler.debugStats.endMarksGBuffer;
  }

  if (!g_gpuProfiler.supported || g_gpuProfiler.writeBlocked) {
    return;
  }

  const RenderDevice *dev = render_device();
  if ((dev == nullptr) || (dev->write_timestamp == nullptr)) {
    return;
  }

  const std::size_t idx = pass_index(pass);
  if (idx >= kPassCount) {
    return;
  }
  QueryRange &range =
      g_gpuProfiler.queryFrames[g_gpuProfiler.writeFrame].ranges[idx];
  if (!range.beginIssued) {
    return;
  }
  dev->write_timestamp(range.endQuery);
  range.submitted = true;
}

float gpu_profiler_pass_ms(GpuPassId pass) noexcept {
  const std::size_t idx = pass_index(pass);
  return (idx < kPassCount) ? g_gpuProfiler.passDurationsMs[idx] : 0.0F;
}

GpuProfilerDebugStats gpu_profiler_debug_stats() noexcept {
  return g_gpuProfiler.debugStats;
}

} // namespace engine::renderer
