#pragma once

#include <cstdint>

namespace engine::core {

using JobFunction = void (*)(void *) noexcept;

struct Job final {
  JobFunction function = nullptr;
  void *data = nullptr;
};

struct JobHandle final {
  std::uint32_t id = 0U;
};

struct JobSystemStats final {
  std::uint64_t jobsExecuted = 0;
  std::uint64_t busyNanoseconds = 0;
  std::uint64_t queueContentionCount = 0;
};

bool initialize_job_system(std::uint32_t workerCount) noexcept;
void shutdown_job_system() noexcept;
bool is_job_system_initialized() noexcept;

bool begin_frame_graph() noexcept;
bool end_frame_graph() noexcept;

JobHandle submit(Job job) noexcept;
bool add_dependency(JobHandle prerequisite, JobHandle dependent) noexcept;
void wait(JobHandle handle) noexcept;
bool is_valid_handle(JobHandle handle) noexcept;
bool is_completed(JobHandle handle) noexcept;

std::uint32_t worker_count() noexcept;
std::uint32_t thread_count() noexcept;
std::uint32_t current_thread_index() noexcept;

JobSystemStats consume_job_stats() noexcept;

} // namespace engine::core
