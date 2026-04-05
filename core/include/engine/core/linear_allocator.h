#pragma once

#include <cstddef>

#include "engine/core/allocator.h"

namespace engine::core {

class LinearAllocator final {
public:
    void init(void* memory, std::size_t capacityBytes) noexcept;
    void* allocate(std::size_t sizeBytes, std::size_t alignment) noexcept;
    void reset() noexcept;

    std::size_t capacity() const noexcept;
    std::size_t bytes_used() const noexcept;
    std::size_t allocation_count() const noexcept;

private:
    std::byte* m_memory = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_head = 0;
    std::size_t m_allocationCount = 0;
};

Allocator make_allocator(LinearAllocator* allocator) noexcept;

} // namespace engine::core
