// Declares the memory tracker: per-subsystem byte counts that the owners
// of the engine's large pools report to, and the snapshot the editor's
// Stats panel draws. A tag nothing has reported to is unmeasured, which a
// display must say rather than show as zero.

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
  /// True once anything has reported an allocation under the tag. False
  /// means the tag is not measured, not that it holds nothing.
  bool reported = false;
};

/// Fill \p out with snapshots for every tag.  Returns kMemTagCount.
std::size_t mem_tracker_snapshot(MemTagSnapshot *out,
                                 std::size_t maxEntries) noexcept;

/// Return the current byte count for a single tag.  Thread-safe.
std::int64_t mem_tracker_current_bytes(MemTag tag) noexcept;

/// Human-readable name for a tag.
const char *mem_tag_name(MemTag tag) noexcept;

/// Reports a fixed-size pool's bytes under a tag for as long as it lives:
/// the pool's owner holds one beside the pool, and the bytes are released
/// when the owner goes, however it goes. Not copyable, so a report is
/// never released twice.
class MemReport final {
public:
  MemReport() noexcept = default;
  MemReport(const MemReport &) = delete;
  MemReport &operator=(const MemReport &) = delete;
  ~MemReport() noexcept { release(); }

  /// Replaces whatever this report held with `bytes` under `tag`.
  void report(MemTag tag, std::size_t bytes) noexcept {
    release();
    mem_tracker_alloc(tag, bytes);
    m_tag = tag;
    m_bytes = bytes;
  }

  /// Releases what this report holds; nothing when it holds nothing.
  void release() noexcept {
    if (m_bytes != 0U) {
      mem_tracker_free(m_tag, m_bytes);
      m_bytes = 0U;
    }
  }

private:
  MemTag m_tag = MemTag::General;
  std::size_t m_bytes = 0U;
};

} // namespace engine::core
