// Declares pool allocator types and APIs for the Engine core engine.

#pragma once
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace engine::core {

// Fixed-size pool allocator for T objects. Capacity is set at construction.
// No heap allocation after initialization; uses a pre-allocated buffer.
// Thread-UNSAFE — intended for single-threaded use or external locking.
template <typename T, std::size_t Capacity>
class PoolAllocator final {
public:
  static_assert(Capacity > 0U, "PoolAllocator capacity must be > 0");
  static_assert(std::is_nothrow_default_constructible_v<T>,
                "PoolAllocator<T> requires nothrow default construction");
  static_assert(std::is_nothrow_destructible_v<T>,
                "PoolAllocator<T> requires nothrow destruction");

  PoolAllocator() noexcept {
    // Build free list: each slot points to the next free slot.
    for (std::size_t i = 0U; i < Capacity - 1U; ++i) {
      m_storage[i].next = &m_storage[i + 1U];
    }
    m_storage[Capacity - 1U].next = nullptr;
    m_freeHead = &m_storage[0];
  }

  ~PoolAllocator() noexcept {
    for (std::size_t i = 0U; i < Capacity; ++i) {
      if (m_occupied[i]) {
        object_at(i)->~T();
        m_occupied[i] = false;
      }
    }
  }

  PoolAllocator(const PoolAllocator &) = delete;
  PoolAllocator &operator=(const PoolAllocator &) = delete;
  PoolAllocator(PoolAllocator &&) = delete;
  PoolAllocator &operator=(PoolAllocator &&) = delete;

  // Allocate one slot. Returns nullptr if pool is exhausted.
  T *allocate() noexcept {
    if (m_freeHead == nullptr) {
      return nullptr;
    }
    Slot *slot = m_freeHead;
    m_freeHead = slot->next;
    const auto index = static_cast<std::size_t>(slot - m_storage);
    m_occupied[index] = true;
    return new (static_cast<void *>(&slot->storage[0])) T();
  }

  // Return a slot to the pool. Pointer must have been returned by allocate().
  void deallocate(T *ptr) noexcept {
    if (ptr == nullptr) {
      return;
    }
    for (std::size_t i = 0U; i < Capacity; ++i) {
      if (ptr == object_at(i)) {
        if (!m_occupied[i]) {
          return;
        }
        ptr->~T();
        m_occupied[i] = false;
        m_storage[i].next = m_freeHead;
        m_freeHead = &m_storage[i];
        return;
      }
    }
  }

  std::size_t capacity() const noexcept { return Capacity; }

private:
  union Slot final {
    Slot() noexcept : next(nullptr) {}
    ~Slot() noexcept {}

    Slot *next;
    alignas(T) std::byte storage[sizeof(T)];
  };

  T *object_at(std::size_t index) noexcept {
    return reinterpret_cast<T *>(&m_storage[index].storage[0]);
  }

  Slot m_storage[Capacity]{};
  bool m_occupied[Capacity]{};
  Slot *m_freeHead = nullptr;
};

} // namespace engine::core
