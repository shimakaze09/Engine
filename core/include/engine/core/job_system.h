// Declares job system types and APIs for the Engine core engine.

#pragma once

#include <cstdint>

namespace engine::core {

using JobFunction = void (*)(void *) noexcept;

/// One unit of work: function pointer plus opaque data.
struct Job final {
  JobFunction function = nullptr;
  void *data = nullptr;
};

/// Generation-encoded id of a submitted job node.
struct JobHandle final {
  std::uint32_t id = 0U;
};

/// Counters drained by consume_job_stats.
struct JobSystemStats final {
  std::uint64_t jobsExecuted = 0;
  std::uint64_t busyNanoseconds = 0;
  std::uint64_t queueContentionCount = 0;
};

/// Initializes the owning system for job system.
bool initialize_job_system(std::uint32_t workerCount) noexcept;
/// Shuts down the owning system for job system.
void shutdown_job_system() noexcept;
/// Returns whether is job system initialized.
bool is_job_system_initialized() noexcept;

/// Begins the requested operation or profiling range for frame graph.
bool begin_frame_graph() noexcept;
/// Ends the requested operation or profiling range for frame graph.
bool end_frame_graph() noexcept;

/// Submits work to the owning buffer or system.
JobHandle submit(Job job) noexcept;
/// Orders prerequisite before dependent; false for invalid handles.
bool add_dependency(JobHandle prerequisite, JobHandle dependent) noexcept;
/// Blocks until the job completes, executing ready jobs meanwhile.
void wait(JobHandle handle) noexcept;
/// Returns whether is valid handle.
bool is_valid_handle(JobHandle handle) noexcept;
/// Returns whether is completed.
bool is_completed(JobHandle handle) noexcept;

/// Number of worker threads (excludes the main thread).
std::uint32_t worker_count() noexcept;
/// worker_count() plus the main thread.
std::uint32_t thread_count() noexcept;
/// Stable index of the calling thread (0 = main).
std::uint32_t current_thread_index() noexcept;

/// Returns and resets the accumulated stats.
JobSystemStats consume_job_stats() noexcept;

} // namespace engine::core
