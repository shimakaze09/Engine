#pragma once

#include <cstddef>

#include "engine/core/allocator.h"

namespace engine::core {

bool initialize_core(std::size_t frameAllocatorBytes) noexcept;
void shutdown_core() noexcept;
bool is_core_initialized() noexcept;

Allocator frame_allocator() noexcept;
Allocator thread_frame_allocator(std::size_t threadIndex) noexcept;
void reset_frame_allocator() noexcept;
void reset_thread_frame_allocators() noexcept;
std::size_t frame_allocator_bytes_used() noexcept;
std::size_t frame_allocator_allocation_count() noexcept;
std::size_t thread_frame_allocator_bytes_used(std::size_t threadIndex) noexcept;
std::size_t thread_frame_allocator_allocation_count(
    std::size_t threadIndex) noexcept;
std::size_t thread_frame_allocator_count() noexcept;

}  // namespace engine::core
