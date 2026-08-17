// Generational slot table backing render-device resource handles: packs
// slot+generation into the public 32-bit handle values (the same bounded
// bit-layout scheme as texture_handle_codec.h) so a destroyed handle is
// detected as stale instead of aliasing whatever resource reuses its slot.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace engine::renderer::device_slot_detail {

/// Fixed-capacity generational resource table. Payload must be default-
/// constructible; Capacity counts usable slots (slot 0 stays reserved as
/// the invalid handle encoding).
template <typename Payload, std::size_t Capacity> class DeviceSlotTable final {
public:
  static_assert(Capacity >= 2U, "capacity must leave a usable slot");

  static constexpr unsigned kSlotBits = std::bit_width(Capacity - 1U);
  static constexpr std::uint32_t kSlotMask = (1U << kSlotBits) - 1U;
  static constexpr std::uint32_t kGenerationMask =
      std::numeric_limits<std::uint32_t>::max() >> kSlotBits;

  /// Claims a slot for payload; returns the encoded handle value or 0
  /// when the table is full.
  std::uint32_t allocate(const Payload &payload) noexcept {
    std::size_t slot = 0U;
    if (m_freeCount > 0U) {
      --m_freeCount;
      slot = m_freeList[m_freeCount];
    } else if (m_highWater < Capacity) {
      slot = m_highWater;
      ++m_highWater;
    } else {
      return 0U;
    }
    Entry &entry = m_entries[slot];
    entry.payload = payload;
    entry.live = true;
    return encode(slot, entry.generation);
  }

  /// Live payload behind the handle value; nullptr when the handle is
  /// invalid or stale (slot released or reused under a newer generation).
  Payload *resolve(std::uint32_t handleValue) noexcept {
    const std::size_t slot = handleValue & kSlotMask;
    const std::uint32_t generation = handleValue >> kSlotBits;
    if ((slot == 0U) || (slot >= Capacity) || (generation == 0U)) {
      return nullptr;
    }
    Entry &entry = m_entries[slot];
    if (!entry.live || (entry.generation != generation)) {
      return nullptr;
    }
    return &entry.payload;
  }

  /// Releases the slot behind the handle and bumps its generation so
  /// surviving handle copies fail resolve; false when already stale.
  bool release(std::uint32_t handleValue) noexcept {
    if (resolve(handleValue) == nullptr) {
      return false;
    }
    const std::size_t slot = handleValue & kSlotMask;
    Entry &entry = m_entries[slot];
    entry.live = false;
    entry.payload = Payload{};
    entry.generation = next_generation(entry.generation);
    m_freeList[m_freeCount] = slot;
    ++m_freeCount;
    return true;
  }

  /// Invalidates every live entry (shutdown); generations advance so all
  /// outstanding handles turn stale.
  void clear() noexcept {
    for (std::size_t slot = 1U; slot < m_highWater; ++slot) {
      Entry &entry = m_entries[slot];
      if (entry.live) {
        entry.live = false;
        entry.payload = Payload{};
        entry.generation = next_generation(entry.generation);
      }
    }
    m_highWater = 1U;
    m_freeCount = 0U;
  }

  /// Number of live entries (diagnostics/tests).
  std::size_t live_count() const noexcept {
    std::size_t count = 0U;
    for (std::size_t slot = 1U; slot < m_highWater; ++slot) {
      count += m_entries[slot].live ? 1U : 0U;
    }
    return count;
  }

  /// Visits every live payload (backend-internal sweeps).
  template <typename Visitor> void for_each_live(Visitor &&visitor) noexcept {
    for (std::size_t slot = 1U; slot < m_highWater; ++slot) {
      if (m_entries[slot].live) {
        visitor(m_entries[slot].payload);
      }
    }
  }

private:
  struct Entry final {
    Payload payload{};
    std::uint32_t generation = 1U;
    bool live = false;
  };

  /// Advances within the bounded generation field and skips zero.
  static constexpr std::uint32_t
  next_generation(std::uint32_t generation) noexcept {
    return (generation >= kGenerationMask) ? 1U : (generation + 1U);
  }

  /// Packs slot and generation into one nonzero handle value.
  static constexpr std::uint32_t encode(std::size_t slot,
                                        std::uint32_t generation) noexcept {
    return (generation << kSlotBits) | static_cast<std::uint32_t>(slot);
  }

  std::array<Entry, Capacity> m_entries{};
  std::array<std::uint32_t, Capacity> m_freeList{};
  std::size_t m_freeCount = 0U;
  // Slot 0 encodes the invalid handle, so allocation starts at 1.
  std::size_t m_highWater = 1U;
};

} // namespace engine::renderer::device_slot_detail
