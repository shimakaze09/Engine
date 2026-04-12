#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Per-subsystem memory tags for tracking allocations.
enum class MemTag : std::uint8_t {
  Physics = 0,
  Renderer = 1,
  Audio = 2,
  Scripting = 3,
  ECS = 4,
  Assets = 5,
  General = 6,
  Count // sentinel — must be last
};

/// Number of distinct memory tags.
inline constexpr std::size_t kMemTagCount =
    static_cast<std::size_t>(MemTag::Count);

/// Initialize the memory tracker (zeroes all counters).
void mem_tracker_init() noexcept;

/// Record an allocation of \p bytes under \p tag.  Thread-safe.
void mem_tracker_alloc(MemTag tag, std::size_t bytes) noexcept;

/// Record a deallocation of \p bytes under \p tag.  Thread-safe.
void mem_tracker_free(MemTag tag, std::size_t bytes) noexcept;

/// Snapshot of one tag's current state.
struct MemTagSnapshot {
  MemTag tag = MemTag::General;
  std::int64_t currentBytes = 0;
  std::uint64_t totalAllocated = 0;
  std::uint64_t totalFreed = 0;
};

/// Fill \p out with snapshots for every tag.  Returns kMemTagCount.
std::size_t mem_tracker_snapshot(MemTagSnapshot *out,
                                 std::size_t maxEntries) noexcept;

/// Return the current byte count for a single tag.  Thread-safe.
std::int64_t mem_tracker_current_bytes(MemTag tag) noexcept;

/// Human-readable name for a tag.
const char *mem_tag_name(MemTag tag) noexcept;

} // namespace engine::core
