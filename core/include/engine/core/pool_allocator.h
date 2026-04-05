#pragma once
#include <cstddef>
#include <cstdint>

namespace engine::core {

// Fixed-size pool allocator for T objects. Capacity is set at construction.
// No heap allocation after initialization; uses a pre-allocated buffer.
// Thread-UNSAFE — intended for single-threaded use or external locking.
template <typename T, std::size_t Capacity>
class PoolAllocator final {
public:
  static_assert(Capacity > 0U, "PoolAllocator capacity must be > 0");
  static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

  PoolAllocator() noexcept {
    // Build free list: each slot points to the next free slot.
    for (std::size_t i = 0U; i < Capacity - 1U; ++i) {
      *reinterpret_cast<void**>(&m_storage[i]) =
          reinterpret_cast<void*>(&m_storage[i + 1U]);
    }
    *reinterpret_cast<void**>(&m_storage[Capacity - 1U]) = nullptr;
    m_freeHead = reinterpret_cast<void*>(&m_storage[0]);
  }

  // Allocate one slot. Returns nullptr if pool is exhausted.
  T *allocate() noexcept {
    if (m_freeHead == nullptr) {
      return nullptr;
    }
    void *slot = m_freeHead;
    m_freeHead = *reinterpret_cast<void**>(slot);
    return reinterpret_cast<T*>(slot);
  }

  // Return a slot to the pool. Pointer must have been returned by allocate().
  void deallocate(T *ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }
    *reinterpret_cast<void**>(ptr) = m_freeHead;
    m_freeHead = reinterpret_cast<void*>(ptr);
  }

  std::size_t capacity() const noexcept { return Capacity; }

private:
  // Storage: array of Capacity T-sized, T-aligned slots.
  alignas(alignof(T)) std::byte m_storage[Capacity][sizeof(T)];
  void *m_freeHead = nullptr;
};

} // namespace engine::core
