// Fixed-capacity open-addressing hash map with tombstone deletion, shared by
// engine systems that previously hand-rolled probe loops. No heap allocation;
// linear probing; integral keys.

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>

namespace engine::core {

/// Fixed-capacity Key -> Value map. Erase leaves tombstones that inserts
/// reuse; owners should watch tombstone_count() and rebuild from their source
/// of truth when it grows large (probe chains stop only at never-used slots).
template <typename Key, typename Value, std::size_t Capacity>
  requires std::unsigned_integral<Key> && (Capacity > 0U)
class FixedHashTable final {
public:
  /// Inserts or overwrites the value for a key; false only when full.
  bool insert(Key key, const Value &value) noexcept {
    std::size_t tombstone = Capacity;
    std::size_t slot = home_slot(key);
    for (std::size_t probe = 0U; probe < Capacity; ++probe) {
      const std::uint8_t state = m_states[slot];
      if (state == kEmpty) {
        const std::size_t target = (tombstone != Capacity) ? tombstone : slot;
        if (target == tombstone) {
          --m_tombstones;
        }
        m_states[target] = kOccupied;
        m_keys[target] = key;
        m_values[target] = value;
        ++m_size;
        return true;
      }

      if (state == kTombstone) {
        if (tombstone == Capacity) {
          tombstone = slot;
        }
      } else if (m_keys[slot] == key) {
        m_values[slot] = value;
        return true;
      }

      slot = (slot + 1U) % Capacity;
    }

    if (tombstone != Capacity) {
      m_states[tombstone] = kOccupied;
      m_keys[tombstone] = key;
      m_values[tombstone] = value;
      --m_tombstones;
      ++m_size;
      return true;
    }

    return false;
  }

  /// Returns the value for a key or nullptr when absent.
  const Value *find(Key key) const noexcept {
    const std::size_t slot = find_slot(key);
    return (slot != Capacity) ? &m_values[slot] : nullptr;
  }

  /// Returns the mutable value for a key or nullptr when absent.
  Value *find(Key key) noexcept {
    const std::size_t slot = find_slot(key);
    return (slot != Capacity) ? &m_values[slot] : nullptr;
  }

  /// Tombstones the entry for a key; false when absent.
  bool erase(Key key) noexcept {
    const std::size_t slot = find_slot(key);
    if (slot == Capacity) {
      return false;
    }

    m_states[slot] = kTombstone;
    m_keys[slot] = Key{};
    m_values[slot] = Value{};
    --m_size;
    ++m_tombstones;
    return true;
  }

  /// Removes every entry and every tombstone.
  void clear() noexcept {
    m_states.fill(kEmpty);
    m_size = 0U;
    m_tombstones = 0U;
  }

  /// Number of live entries.
  std::size_t size() const noexcept { return m_size; }

  /// Number of tombstoned slots (rebuild pressure indicator for owners).
  std::size_t tombstone_count() const noexcept { return m_tombstones; }

  /// Total slot capacity.
  static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
  static constexpr std::uint8_t kEmpty = 0U;
  static constexpr std::uint8_t kOccupied = 1U;
  static constexpr std::uint8_t kTombstone = 2U;

  /// Fibonacci-style multiplicative spread before the modulo.
  static std::size_t home_slot(Key key) noexcept {
    return static_cast<std::size_t>(
               (static_cast<std::uint64_t>(key) * 0x9E3779B97F4A7C15ULL) >>
               32U) %
           Capacity;
  }

  /// Resolves a key to its occupied slot or Capacity when absent.
  std::size_t find_slot(Key key) const noexcept {
    std::size_t slot = home_slot(key);
    for (std::size_t probe = 0U; probe < Capacity; ++probe) {
      const std::uint8_t state = m_states[slot];
      if (state == kEmpty) {
        return Capacity;
      }
      if ((state == kOccupied) && (m_keys[slot] == key)) {
        return slot;
      }
      slot = (slot + 1U) % Capacity;
    }
    return Capacity;
  }

  std::array<Key, Capacity> m_keys{};
  std::array<Value, Capacity> m_values{};
  std::array<std::uint8_t, Capacity> m_states{};
  std::size_t m_size = 0U;
  std::size_t m_tombstones = 0U;
};

} // namespace engine::core
