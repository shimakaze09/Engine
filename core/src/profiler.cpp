#include "engine/core/profiler.h"

#include <array>
#include <chrono>

namespace engine::core {

namespace {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

constexpr std::size_t kMaxEntries = 256U;
constexpr std::size_t kMaxDepth = 16U;

struct InternalEntry final {
  const char *name = nullptr;
  TimePoint start{};
  TimePoint end{};
  std::uint32_t depth = 0U;
  std::uint32_t parentIndex = 0xFFFFFFFFU;
};

struct FrameBuffer final {
  std::array<InternalEntry, kMaxEntries> entries{};
  std::size_t count = 0U;
  TimePoint frameStart{};
  TimePoint frameEnd{};
};

bool g_profilerInitialized = false;
FrameBuffer g_writeBuffer{};
FrameBuffer g_readBuffer{};

std::array<std::size_t, kMaxDepth> g_scopeStack{};
std::size_t g_scopeDepth = 0U;

} // namespace

bool initialize_profiler() noexcept {
  if (g_profilerInitialized) {
    return true;
  }

  g_writeBuffer = {};
  g_readBuffer = {};
  g_scopeDepth = 0U;
  g_profilerInitialized = true;
  return true;
}

void shutdown_profiler() noexcept {
  g_profilerInitialized = false;
  g_writeBuffer = {};
  g_readBuffer = {};
  g_scopeDepth = 0U;
}

void profiler_begin_frame() noexcept {
  g_writeBuffer.count = 0U;
  g_writeBuffer.frameStart = Clock::now();
  g_scopeDepth = 0U;
}

void profiler_end_frame() noexcept {
  g_writeBuffer.frameEnd = Clock::now();
  g_readBuffer = g_writeBuffer;
}

void profiler_begin_scope(const char *name) noexcept {
  if (g_writeBuffer.count >= kMaxEntries) {
    return;
  }

  if (g_scopeDepth >= kMaxDepth) {
    return;
  }

  const std::size_t idx = g_writeBuffer.count;
  InternalEntry &entry = g_writeBuffer.entries[idx];
  entry.name = name;
  entry.start = Clock::now();
  entry.depth = static_cast<std::uint32_t>(g_scopeDepth);
  entry.parentIndex =
      (g_scopeDepth > 0U)
          ? static_cast<std::uint32_t>(g_scopeStack[g_scopeDepth - 1U])
          : 0xFFFFFFFFU;

  g_scopeStack[g_scopeDepth] = idx;
  ++g_scopeDepth;
  ++g_writeBuffer.count;
}

void profiler_end_scope() noexcept {
  if (g_scopeDepth == 0U) {
    return;
  }

  --g_scopeDepth;
  const std::size_t idx = g_scopeStack[g_scopeDepth];
  if (idx < g_writeBuffer.count) {
    g_writeBuffer.entries[idx].end = Clock::now();
  }
}

ProfileScope::ProfileScope(const char *name) noexcept {
  profiler_begin_scope(name);
}

ProfileScope::~ProfileScope() noexcept { profiler_end_scope(); }

std::size_t profiler_get_entries(ProfileEntry *out,
                                 std::size_t maxEntries) noexcept {
  if ((out == nullptr) || (maxEntries == 0U)) {
    return 0U;
  }

  const std::size_t count =
      (g_readBuffer.count < maxEntries) ? g_readBuffer.count : maxEntries;
  for (std::size_t i = 0U; i < count; ++i) {
    const InternalEntry &src = g_readBuffer.entries[i];
    out[i].name = src.name;
    out[i].depth = src.depth;
    out[i].parentIndex = src.parentIndex;
    const auto duration =
        std::chrono::duration<float, std::milli>(src.end - src.start);
    out[i].durationMs = duration.count();
  }

  return count;
}

float profiler_frame_time_ms() noexcept {
  const auto duration = std::chrono::duration<float, std::milli>(
      g_readBuffer.frameEnd - g_readBuffer.frameStart);
  return duration.count();
}

bool profiler_compute_flame_starts(const ProfileEntry *entries,
                                   std::size_t count, float *outStartMs,
                                   std::uint32_t *outMaxDepth) noexcept {
  if ((entries == nullptr) || (outStartMs == nullptr)) {
    return false;
  }

  if (count == 0U) {
    if (outMaxDepth != nullptr) {
      *outMaxDepth = 0U;
    }
    return true;
  }

  std::array<float, kMaxEntries> consumedMs{};
  float rootCursorMs = 0.0F;
  std::uint32_t maxDepth = 0U;

  const std::size_t boundedCount = (count < kMaxEntries) ? count : kMaxEntries;
  for (std::size_t i = 0U; i < boundedCount; ++i) {
    const ProfileEntry &entry = entries[i];
    if (entry.depth > maxDepth) {
      maxDepth = entry.depth;
    }

    float thisStartMs = rootCursorMs;
    if ((entry.parentIndex != 0xFFFFFFFFU) &&
        (entry.parentIndex < boundedCount)) {
      thisStartMs =
          outStartMs[entry.parentIndex] + consumedMs[entry.parentIndex];
      consumedMs[entry.parentIndex] += entry.durationMs;
    } else {
      rootCursorMs += entry.durationMs;
    }

    outStartMs[i] = thisStartMs;
  }

  if (outMaxDepth != nullptr) {
    *outMaxDepth = maxDepth;
  }
  return (count <= kMaxEntries);
}

} // namespace engine::core
