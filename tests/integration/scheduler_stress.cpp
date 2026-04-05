#include "engine/core/bootstrap.h"
#include "engine/core/job_system.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::size_t kJobCount = 1500U;

struct StressJobData final {
  std::uint32_t jobIndex = 0U;
  std::uint64_t *output = nullptr;
};

void stress_job(void *userData) noexcept {
  auto *data = static_cast<StressJobData *>(userData);
  if ((data == nullptr) || (data->output == nullptr)) {
    return;
  }

  const std::uint32_t workFactor = (data->jobIndex % 37U) + 1U;
  std::uint64_t value = 0U;

  for (std::uint32_t i = 0U; i < (workFactor * 180U); ++i) {
    value ^= (static_cast<std::uint64_t>(data->jobIndex) * 2654435761ULL) +
             static_cast<std::uint64_t>(i);
    value *= 1099511628211ULL;
  }

  // Create deterministic skew to expose imbalance behavior.
  if ((data->jobIndex % 11U) == 0U) {
    for (std::uint32_t i = 0U; i < 2000U; ++i) {
      value ^= static_cast<std::uint64_t>(i + data->jobIndex);
      value *= 16777619ULL;
    }
  }

  *data->output = value;
}

void completion_job(void *) noexcept {}

std::uint64_t run_stress_round(std::uint64_t *outExecutedJobs,
                               std::uint64_t *outQueueContention) {
  if (!engine::core::begin_frame_graph()) {
    return 0U;
  }

  std::array<StressJobData, kJobCount> jobData{};
  std::array<std::uint64_t, kJobCount> outputs{};

  engine::core::Job completion{};
  completion.function = &completion_job;
  completion.data = nullptr;
  const engine::core::JobHandle completionHandle =
      engine::core::submit(completion);
  if (!engine::core::is_valid_handle(completionHandle)) {
    static_cast<void>(engine::core::end_frame_graph());
    return 0U;
  }

  for (std::size_t i = 0U; i < kJobCount; ++i) {
    jobData[i].jobIndex = static_cast<std::uint32_t>(i);
    jobData[i].output = &outputs[i];

    engine::core::Job job{};
    job.function = &stress_job;
    job.data = &jobData[i];

    const engine::core::JobHandle handle = engine::core::submit(job);
    if (!engine::core::is_valid_handle(handle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 0U;
    }

    if (!engine::core::add_dependency(handle, completionHandle)) {
      static_cast<void>(engine::core::end_frame_graph());
      return 0U;
    }
  }

  engine::core::wait(completionHandle);

  if (!engine::core::end_frame_graph()) {
    return 0U;
  }

  const engine::core::JobSystemStats stats = engine::core::consume_job_stats();
  if (outExecutedJobs != nullptr) {
    *outExecutedJobs = stats.jobsExecuted;
  }
  if (outQueueContention != nullptr) {
    *outQueueContention = stats.queueContentionCount;
  }

  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0U; i < outputs.size(); ++i) {
    hash ^= outputs[i];
    hash *= 1099511628211ULL;
  }

  return hash;
}

} // namespace

int main() {
  if (!engine::core::initialize_core(1024U * 1024U)) {
    return 1;
  }

  std::uint64_t executedJobsRound1 = 0U;
  std::uint64_t queueContentionRound1 = 0U;
  const std::uint64_t hashRound1 =
      run_stress_round(&executedJobsRound1, &queueContentionRound1);

  std::uint64_t executedJobsRound2 = 0U;
  std::uint64_t queueContentionRound2 = 0U;
  const std::uint64_t hashRound2 =
      run_stress_round(&executedJobsRound2, &queueContentionRound2);

  const std::uint32_t observedThreadCount = engine::core::thread_count();

  engine::core::shutdown_core();

  if ((hashRound1 == 0U) || (hashRound2 == 0U)) {
    return 2;
  }

  if (hashRound1 != hashRound2) {
    return 3;
  }

  if ((executedJobsRound1 < (kJobCount + 1U)) ||
      (executedJobsRound2 < (kJobCount + 1U))) {
    return 4;
  }

  if ((queueContentionRound1 == 0U) && (queueContentionRound2 == 0U) &&
      (observedThreadCount > 1U)) {
    return 5;
  }

  return 0;
}
