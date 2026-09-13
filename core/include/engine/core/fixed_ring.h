// Fixed-capacity FIFO ring shared by engine systems that previously carried
// their own head/count arithmetic: elements enter at the back, leave from
// the front, and live in one preallocated array, so a queue on a hot or
// lock-held path never allocates. Push either refuses when full (bounded
// work queues, where a drop must be counted by the owner) or overwrites the
// oldest element (retained histories, where the newest entries matter).

#pragma once

#include <array>
#include <cstddef>

namespace engine::core {

/// Bounded FIFO of Capacity elements. Not thread-safe: an owner shared
/// across threads guards it with its own lock, as the console capture
/// does. Indices count from the oldest retained element (0) to the newest
/// (size() - 1) regardless of where the ring has wrapped.
template <typename T, std::size_t Capacity>
  requires(Capacity > 0U)
class FixedRing final {
public:
  static constexpr std::size_t kCapacity = Capacity;

  /// Number of elements currently held.
  std::size_t size() const noexcept { return m_count; }
  bool empty() const noexcept { return m_count == 0U; }
  bool full() const noexcept { return m_count == Capacity; }

  /// Appends at the back; false, with nothing written, when full.
  bool push(const T &value) noexcept {
    if (full()) {
      return false;
    }
    m_slots[(m_head + m_count) % Capacity] = value;
    ++m_count;
    return true;
  }

  /// Appends at the back, discarding the oldest element when full; true
  /// when an element was discarded to make room.
  bool push_overwrite(const T &value) noexcept {
    const bool dropped = full();
    m_slots[(m_head + m_count) % Capacity] = value;
    if (dropped) {
      m_head = (m_head + 1U) % Capacity;
    } else {
      ++m_count;
    }
    return dropped;
  }

  /// Moves the oldest element into *out and resets its slot to T{}, so a
  /// vacated slot never holds stale bytes; false when empty or out is null.
  bool pop(T *out) noexcept {
    if ((out == nullptr) || empty()) {
      return false;
    }
    *out = m_slots[m_head];
    m_slots[m_head] = T{};
    m_head = (m_head + 1U) % Capacity;
    --m_count;
    return true;
  }

  /// Element `index` counted from the oldest; nullptr when out of range.
  T *at(std::size_t index) noexcept {
    return (index < m_count) ? &m_slots[(m_head + index) % Capacity]
                             : nullptr;
  }
  const T *at(std::size_t index) const noexcept {
    return (index < m_count) ? &m_slots[(m_head + index) % Capacity]
                             : nullptr;
  }

  /// Newest element; nullptr when empty.
  T *back() noexcept { return empty() ? nullptr : at(m_count - 1U); }
  const T *back() const noexcept {
    return empty() ? nullptr : at(m_count - 1U);
  }

  /// Empties the ring and resets every slot to T{} one element at a time,
  /// so no whole-array temporary is ever materialized (a T of several
  /// hundred bytes times a large Capacity would exceed a small thread
  /// stack if the compiler chose not to elide one).
  void clear() noexcept {
    for (T &slot : m_slots) {
      slot = T{};
    }
    m_head = 0U;
    m_count = 0U;
  }

private:
  std::array<T, Capacity> m_slots{};
  std::size_t m_head = 0U;
  std::size_t m_count = 0U;
};

} // namespace engine::core
