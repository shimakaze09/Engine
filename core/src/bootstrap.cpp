#include "engine/core/bootstrap.h"

#include <array>
#include <cstddef>
#include <thread>

#include "engine/core/event_bus.h"
#include "engine/core/input.h"
#include "engine/core/job_system.h"
#include "engine/core/linear_allocator.h"
#include "engine/core/logging.h"
#include "engine/core/platform.h"
#include "engine/core/profiler.h"
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

  if ((frameAllocatorBytes == 0U) ||
      (frameAllocatorBytes > kMaxFrameAllocatorBytes)) {
    log_message(LogLevel::Error, "core",
                "invalid frame allocator size for core initialization");
    return false;
  }

  bool loggingInitialized = false;
  bool vfsInitialized = false;
  bool eventBusInitialized = false;
  bool platformInitialized = false;
  bool inputInitialized = false;
  bool profilerInitialized = false;
  bool jobSystemInitialized = false;
  bool initializedSuccessfully = false;
  const char *failureMessage = nullptr;

  g_mainFrameAllocator.init(g_mainFrameAllocatorMemory.data(),
                            frameAllocatorBytes);
  g_mainFrameAllocatorInterface = make_allocator(&g_mainFrameAllocator);

  do {
    if (!initialize_logging()) {
      failureMessage = "failed to initialize logging";
      break;
    }
    loggingInitialized = true;

    if (!initialize_vfs()) {
      failureMessage = "failed to initialize virtual file system";
      break;
    }
    vfsInitialized = true;

    if (!initialize_event_bus()) {
      failureMessage = "failed to initialize event bus";
      break;
    }
    eventBusInitialized = true;

    if (!initialize_platform()) {
      failureMessage = "failed to initialize platform";
      break;
    }
    platformInitialized = true;

    if (!initialize_input()) {
      failureMessage = "failed to initialize input";
      break;
    }
    inputInitialized = true;

    if (!initialize_profiler()) {
      failureMessage = "failed to initialize profiler";
      break;
    }
    profilerInitialized = true;

    const std::uint32_t hardwareThreads = std::thread::hardware_concurrency();
    const std::uint32_t workerThreads =
        (hardwareThreads > 1U) ? (hardwareThreads - 1U) : 0U;
    if (!initialize_job_system(workerThreads)) {
      failureMessage = "failed to initialize job system";
      break;
    }
    jobSystemInitialized = true;

    g_threadFrameAllocatorCount = static_cast<std::size_t>(thread_count());
    if ((g_threadFrameAllocatorCount == 0U) ||
        (g_threadFrameAllocatorCount > kMaxThreadFrameAllocators)) {
      failureMessage = "thread frame allocator count is out of range";
      break;
    }

    for (std::size_t i = 0U; i < g_threadFrameAllocatorCount; ++i) {
      g_threadFrameAllocators[i].init(g_threadFrameAllocatorMemory[i].data(),
                                      kThreadFrameAllocatorBytes);
      g_threadFrameAllocatorInterfaces[i] =
          make_allocator(&g_threadFrameAllocators[i]);
    }

    initializedSuccessfully = true;
  } while (false);

  if (initializedSuccessfully) {
    g_coreInitialized = true;
    log_message(LogLevel::Info, "core", "core initialized");
    return true;
  }

  if (loggingInitialized && (failureMessage != nullptr)) {
    log_message(LogLevel::Error, "core", failureMessage);
  }

  if (jobSystemInitialized) {
    shutdown_job_system();
  }

  if (profilerInitialized) {
    shutdown_profiler();
  }

  if (inputInitialized) {
    shutdown_input();
  }

  if (platformInitialized) {
    shutdown_platform();
  }

  if (eventBusInitialized) {
    shutdown_event_bus();
  }

  if (vfsInitialized) {
    shutdown_vfs();
  }

  if (loggingInitialized) {
    shutdown_logging();
  }

  g_mainFrameAllocator.reset();
  for (std::size_t i = 0U; i < kMaxThreadFrameAllocators; ++i) {
    g_threadFrameAllocators[i].reset();
    g_threadFrameAllocatorInterfaces[i] = Allocator{};
  }
  g_threadFrameAllocatorCount = 1U;

  return false;
}

void shutdown_core() noexcept {
  if (!g_coreInitialized) {
    return;
  }

  shutdown_job_system();
  shutdown_profiler();
  shutdown_input();
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

bool is_core_initialized() noexcept { return g_coreInitialized; }

Allocator frame_allocator() noexcept { return g_mainFrameAllocatorInterface; }

Allocator thread_frame_allocator(std::size_t threadIndex) noexcept {
  if (threadIndex >= g_threadFrameAllocatorCount) {
    return Allocator{};
  }

  return g_threadFrameAllocatorInterfaces[threadIndex];
}

void reset_frame_allocator() noexcept { g_mainFrameAllocator.reset(); }

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
