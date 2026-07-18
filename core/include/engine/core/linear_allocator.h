// Declares linear allocator types and APIs for the Engine core engine.

#pragma once

#include <cstddef>

#include "engine/core/allocator.h"

namespace engine::core {

/// Bump allocator over a fixed buffer; reset frees everything at once.
class LinearAllocator final {
public:
    void init(void* memory, std::size_t capacityBytes) noexcept;
    /// Bump-allocates sizeBytes at alignment; nullptr when exhausted.
    void* allocate(std::size_t sizeBytes, std::size_t alignment) noexcept;
    /// Resets this object back to its reusable empty state.
    void reset() noexcept;

    /// Total buffer capacity in bytes.
    std::size_t capacity() const noexcept;
    /// Bytes handed out since the last reset.
    std::size_t bytes_used() const noexcept;
    /// Allocations served since the last reset.
    std::size_t allocation_count() const noexcept;

private:
    std::byte* m_memory = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_head = 0;
    std::size_t m_allocationCount = 0;
};

/// Wraps the LinearAllocator in the type-erased Allocator interface.
Allocator make_allocator(LinearAllocator* allocator) noexcept;

} // namespace engine::core
