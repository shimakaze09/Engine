// Declares bootstrap types and APIs for the Engine core engine.

#pragma once

#include <cstddef>

#include "engine/core/allocator.h"
#include "engine/core/platform.h"

namespace engine::core {

/// Describes core-owned subsystem startup choices.
struct CoreConfig final {
  std::size_t frameAllocatorBytes = 1024U * 1024U;
  bool initializePlatform = true;
  PlatformConfig platform{};
};

/// Initializes the owning system for core.
bool initialize_core(std::size_t frameAllocatorBytes) noexcept;
/// Initializes the owning system for core with explicit subsystem ownership.
bool initialize_core(const CoreConfig &config) noexcept;
/// Shuts down the owning system for core.
void shutdown_core() noexcept;
/// Returns whether is core initialized.
bool is_core_initialized() noexcept;

/// Main-thread frame-scoped allocator (reset each frame).
Allocator frame_allocator() noexcept;
/// Frame-scoped allocator for the given worker thread.
Allocator thread_frame_allocator(std::size_t threadIndex) noexcept;
/// Resets this object back to its reusable empty state for frame allocator.
void reset_frame_allocator() noexcept;
/// Resets this object back to its reusable empty state for thread frame allocators.
void reset_thread_frame_allocators() noexcept;
/// Bytes served by the main frame allocator this frame.
std::size_t frame_allocator_bytes_used() noexcept;
/// Allocations served by the main frame allocator this frame.
std::size_t frame_allocator_allocation_count() noexcept;
/// Bytes served by a worker's frame allocator this frame.
std::size_t thread_frame_allocator_bytes_used(std::size_t threadIndex) noexcept;
/// Allocations served by a worker's frame allocator this frame.
std::size_t thread_frame_allocator_allocation_count(
    std::size_t threadIndex) noexcept;
/// Number of per-worker frame allocators.
std::size_t thread_frame_allocator_count() noexcept;

}  // namespace engine::core
