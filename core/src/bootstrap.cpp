#include "engine/core/bootstrap.h"

#include <array>
#include <cstddef>
#include <thread>

#include "engine/core/event_bus.h"
#include "engine/core/job_system.h"
#include "engine/core/linear_allocator.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/vfs.h"

namespace engine::core {

namespace {

constexpr std::size_t kMaxFrameAllocatorBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxThreadFrameAllocators = 16U;
constexpr std::size_t kThreadFrameAllocatorBytes = 256U * 1024U;

bool g_coreInitialized = false;
LinearAllocator g_mainFrameAllocator;
Allocator g_mainFrameAllocatorInterface{};
std::array<std::byte, kMaxFrameAllocatorBytes> g_mainFrameAllocatorMemory{};

std::array<LinearAllocator, kMaxThreadFrameAllocators>
    g_threadFrameAllocators{};
std::array<Allocator, kMaxThreadFrameAllocators>
    g_threadFrameAllocatorInterfaces{};
std::array<std::array<std::byte, kThreadFrameAllocatorBytes>,
           kMaxThreadFrameAllocators>
    g_threadFrameAllocatorMemory{};
std::size_t g_threadFrameAllocatorCount = 1U;

} // namespace

bool initialize_core(std::size_t frameAllocatorBytes) noexcept {
  if (g_coreInitialized) {
    return true;
  }

  if ((frameAllocatorBytes == 0U)
      || (frameAllocatorBytes > kMaxFrameAllocatorBytes)) {
    return false;
  }

  g_mainFrameAllocator.init(g_mainFrameAllocatorMemory.data(),
                            frameAllocatorBytes);
  g_mainFrameAllocatorInterface = make_allocator(&g_mainFrameAllocator);

  if (!initialize_logging()) {
    return false;
  }

  if (!initialize_vfs()) {
    shutdown_logging();
    return false;
  }

  if (!initialize_event_bus()) {
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  if (!initialize_platform()) {
    shutdown_event_bus();
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  const std::uint32_t hardwareThreads = std::thread::hardware_concurrency();
  const std::uint32_t workerThreads =
      (hardwareThreads > 1U) ? (hardwareThreads - 1U) : 0U;
  if (!initialize_job_system(workerThreads)) {
    shutdown_platform();
    shutdown_event_bus();
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  g_threadFrameAllocatorCount = static_cast<std::size_t>(thread_count());
  if ((g_threadFrameAllocatorCount == 0U)
      || (g_threadFrameAllocatorCount > kMaxThreadFrameAllocators)) {
    shutdown_job_system();
    shutdown_platform();
    shutdown_event_bus();
    shutdown_vfs();
    shutdown_logging();
    return false;
  }

  for (std::size_t i = 0U; i < g_threadFrameAllocatorCount; ++i) {
    g_threadFrameAllocators[i].init(g_threadFrameAllocatorMemory[i].data(),
                                    kThreadFrameAllocatorBytes);
    g_threadFrameAllocatorInterfaces[i] =
        make_allocator(&g_threadFrameAllocators[i]);
  }

  g_coreInitialized = true;
  log_message(LogLevel::Info, "core", "core initialized");
  return true;
}

void shutdown_core() noexcept {
  if (!g_coreInitialized) {
    return;
  }

  shutdown_job_system();
  shutdown_platform();
  shutdown_event_bus();
  shutdown_vfs();
  shutdown_logging();

  g_mainFrameAllocator.reset();
  for (std::size_t i = 0U; i < g_threadFrameAllocatorCount; ++i) {
    g_threadFrameAllocators[i].reset();
  }

  g_coreInitialized = false;
}

bool is_core_initialized() noexcept {
  return g_coreInitialized;
}

Allocator frame_allocator() noexcept {
  return g_mainFrameAllocatorInterface;
}

Allocator thread_frame_allocator(std::size_t threadIndex) noexcept {
  if (threadIndex >= g_threadFrameAllocatorCount) {
    return Allocator{};
  }

  return g_threadFrameAllocatorInterfaces[threadIndex];
}

void reset_frame_allocator() noexcept {
  g_mainFrameAllocator.reset();
}

void reset_thread_frame_allocators() noexcept {
  for (std::size_t i = 0U; i < g_threadFrameAllocatorCount; ++i) {
    g_threadFrameAllocators[i].reset();
  }
}

std::size_t frame_allocator_bytes_used() noexcept {
  return g_mainFrameAllocator.bytes_used();
}

std::size_t frame_allocator_allocation_count() noexcept {
  return g_mainFrameAllocator.allocation_count();
}

std::size_t
thread_frame_allocator_bytes_used(std::size_t threadIndex) noexcept {
  if (threadIndex >= g_threadFrameAllocatorCount) {
    return 0U;
  }

  return g_threadFrameAllocators[threadIndex].bytes_used();
}

std::size_t
thread_frame_allocator_allocation_count(std::size_t threadIndex) noexcept {
  if (threadIndex >= g_threadFrameAllocatorCount) {
    return 0U;
  }

  return g_threadFrameAllocators[threadIndex].allocation_count();
}

std::size_t thread_frame_allocator_count() noexcept {
  return g_threadFrameAllocatorCount;
}

} // namespace engine::core
