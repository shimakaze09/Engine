#include "engine/core/mem_tracker.h"

#include <array>
#include <atomic>


namespace engine::core {

namespace {

struct TagCounters {
  std::atomic<std::int64_t> currentBytes{0};
  std::atomic<std::uint64_t> totalAllocated{0};
  std::atomic<std::uint64_t> totalFreed{0};
};

std::array<TagCounters, kMemTagCount> g_tagCounters{};

} // namespace

void mem_tracker_init() noexcept {
  for (auto &c : g_tagCounters) {
    c.currentBytes.store(0, std::memory_order_relaxed);
    c.totalAllocated.store(0, std::memory_order_relaxed);
    c.totalFreed.store(0, std::memory_order_relaxed);
  }
}

void mem_tracker_alloc(MemTag tag, std::size_t bytes) noexcept {
  const auto idx = static_cast<std::size_t>(tag);
  if (idx >= kMemTagCount) {
    return;
  }
  g_tagCounters[idx].currentBytes.fetch_add(static_cast<std::int64_t>(bytes),
                                            std::memory_order_relaxed);
  g_tagCounters[idx].totalAllocated.fetch_add(static_cast<std::uint64_t>(bytes),
                                              std::memory_order_relaxed);
}

void mem_tracker_free(MemTag tag, std::size_t bytes) noexcept {
  const auto idx = static_cast<std::size_t>(tag);
  if (idx >= kMemTagCount) {
    return;
  }
  g_tagCounters[idx].currentBytes.fetch_sub(static_cast<std::int64_t>(bytes),
                                            std::memory_order_relaxed);
  g_tagCounters[idx].totalFreed.fetch_add(static_cast<std::uint64_t>(bytes),
                                          std::memory_order_relaxed);
}

std::size_t mem_tracker_snapshot(MemTagSnapshot *out,
                                 std::size_t maxEntries) noexcept {
  if (out == nullptr) {
    return 0U;
  }
  const std::size_t count =
      (maxEntries < kMemTagCount) ? maxEntries : kMemTagCount;
  for (std::size_t i = 0U; i < count; ++i) {
    out[i].tag = static_cast<MemTag>(i);
    out[i].currentBytes =
        g_tagCounters[i].currentBytes.load(std::memory_order_relaxed);
    out[i].totalAllocated =
        g_tagCounters[i].totalAllocated.load(std::memory_order_relaxed);
    out[i].totalFreed =
        g_tagCounters[i].totalFreed.load(std::memory_order_relaxed);
  }
  return count;
}

std::int64_t mem_tracker_current_bytes(MemTag tag) noexcept {
  const auto idx = static_cast<std::size_t>(tag);
  if (idx >= kMemTagCount) {
    return 0;
  }
  return g_tagCounters[idx].currentBytes.load(std::memory_order_relaxed);
}

const char *mem_tag_name(MemTag tag) noexcept {
  switch (tag) {
  case MemTag::Physics:
    return "Physics";
  case MemTag::Renderer:
    return "Renderer";
  case MemTag::Audio:
    return "Audio";
  case MemTag::Scripting:
    return "Scripting";
  case MemTag::ECS:
    return "ECS";
  case MemTag::Assets:
    return "Assets";
  case MemTag::General:
    return "General";
  default:
    return "Unknown";
  }
}

} // namespace engine::core
