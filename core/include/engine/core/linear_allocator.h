// Declares linear allocator types and APIs for the Engine core engine.

#pragma once

#include <cstddef>

#include "engine/core/allocator.h"

namespace engine::core {

/// Owns the linear allocator behavior and state.
class LinearAllocator final {
/// Handles init.
public:
    /// Handles init.
    void init(void* memory, std::size_t capacityBytes) noexcept;
    /// Handles allocate.
    void* allocate(std::size_t sizeBytes, std::size_t alignment) noexcept;
    /// Resets this object back to its reusable empty state.
    void reset() noexcept;

    /// Handles capacity.
    std::size_t capacity() const noexcept;
    /// Handles bytes used.
    std::size_t bytes_used() const noexcept;
    /// Handles allocation count.
    std::size_t allocation_count() const noexcept;

private:
    std::byte* m_memory = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_head = 0;
    std::size_t m_allocationCount = 0;
};

/// Handles make allocator.
Allocator make_allocator(LinearAllocator* allocator) noexcept;

} // namespace engine::core
