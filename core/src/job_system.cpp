// Implements job system behavior for the Engine core engine.

#include "engine/core/job_system.h"

#include "engine/core/logging.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace engine::core {

namespace {

constexpr std::uint32_t kMaxWorkers = 15U;
constexpr std::size_t kMaxJobs = 8192U;
constexpr std::size_t kMaxEdges = 65536U;
constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFU;
constexpr std::uint32_t kIndexBits = 13U;
constexpr std::uint32_t kIndexMask = (1U << kIndexBits) - 1U;
// A handle packs (generation << kIndexBits) | index into 32 bits, so only
// this many generation bits survive the round trip. The live generation
// counter must stay within this width or every encoded handle would fail
// validation once the counter passes it.
constexpr std::uint32_t kGenerationBits = 32U - kIndexBits;
constexpr std::uint32_t kGenerationMask = (1U << kGenerationBits) - 1U;

static_assert(kMaxJobs <= (1ULL << kIndexBits),
              "job indices must fit the handle index bits");

thread_local std::uint32_t g_threadIndex = 0U;

/// Stores alignas data used by the engine.
struct alignas(64) ThreadStats final {
  std::atomic<std::uint64_t> jobsExecuted = 0U;
  std::atomic<std::uint64_t> busyNanoseconds = 0U;
  // Keep one stats record per cache line without compiler-inserted padding.
  std::array<std::uint8_t, 64U - (2U * sizeof(std::atomic<std::uint64_t>))>
      cacheLinePad{};
};

/// Stores job node data used by the engine.
struct JobNode final {
  Job job{};
  std::atomic<std::uint32_t> remainingDependencies = 0U;
  std::atomic<bool> completed = false;
  std::uint32_t generation = 0U;
  std::uint32_t firstDependentEdge = kInvalidIndex;
  bool active = false;
};

/// Stores dependency edge data used by the engine.
struct DependencyEdge final {
  std::uint32_t dependentIndex = kInvalidIndex;
  std::uint32_t nextEdge = kInvalidIndex;
};

/// Handles encode handle id.
std::uint32_t encode_handle_id(std::uint32_t index,
                               std::uint32_t generation) noexcept {
  return (generation << kIndexBits) | index;
}

/// Handles decode handle index.
std::uint32_t decode_handle_index(JobHandle handle) noexcept {
  return handle.id & kIndexMask;
}

/// Handles decode handle generation.
std::uint32_t decode_handle_generation(JobHandle handle) noexcept {
  return handle.id >> kIndexBits;
}

/// Owns the job system behavior and state.
class JobSystem final {
public:
  bool initialize(std::uint32_t requestedWorkers) noexcept {
    if (m_initialized.load(std::memory_order_acquire)) {
      return true;
    }

    m_workerCount =
        (requestedWorkers > kMaxWorkers) ? kMaxWorkers : requestedWorkers;
    m_running.store(true, std::memory_order_release);
    m_pendingJobs.store(0U, std::memory_order_release);

    {
      std::lock_guard<std::mutex> lock(m_graphMutex);
      reset_graph_state_locked();
      m_graphActive = false;
    }

    for (std::uint32_t i = 0U; i < m_workerCount; ++i) {
      m_workers[i] = std::thread(&JobSystem::worker_loop, this, i + 1U);
    }

    m_initialized.store(true, std::memory_order_release);
    return true;
  }

  void shutdown() noexcept {
    if (!m_initialized.load(std::memory_order_acquire)) {
      return;
    }

    wait_for_all_jobs();

    m_running.store(false, std::memory_order_release);
    {
      // Synchronize with the worker wait predicate: a worker that evaluated
      // the predicate before the store is guaranteed to be inside wait()
      // once this lock is acquired, so the notify below cannot be lost.
      std::lock_guard<std::mutex> lock(m_queueMutex);
    }
    m_workAvailable.notify_all();

    for (std::uint32_t i = 0U; i < m_workerCount; ++i) {
      if (m_workers[i].joinable()) {
        m_workers[i].join();
      }
    }

    {
      std::lock_guard<std::mutex> lock(m_graphMutex);
      reset_graph_state_locked();
      m_graphActive = false;
    }

    m_workerCount = 0U;
    m_initialized.store(false, std::memory_order_release);
  }

  bool is_initialized() const noexcept {
    return m_initialized.load(std::memory_order_acquire);
  }

  bool begin_graph() noexcept {
    if (!is_initialized()) {
      log_message(LogLevel::Error, "jobs",
                  "begin_graph failed: job system not initialized");
      return false;
    }

    std::lock_guard<std::mutex> lock(m_graphMutex);
    if (m_graphActive) {
      log_message(LogLevel::Error, "jobs",
                  "begin_graph failed: previous graph still active");
      return false;
    }

    const auto pending = m_pendingJobs.load(std::memory_order_acquire);
    if (pending != 0U) {
      char msg[128] = {};
      std::snprintf(msg, sizeof(msg),
                    "begin_graph failed: pendingJobs=%llu (expected 0)",
                    static_cast<unsigned long long>(pending));
      log_message(LogLevel::Error, "jobs", msg);
      return false;
    }

    reset_graph_state_locked();
    m_graphActive = true;
    return true;
  }

  bool end_graph() noexcept {
    if (!is_initialized()) {
      return false;
    }

    std::lock_guard<std::mutex> lock(m_graphMutex);
    if (!m_graphActive) {
      return false;
    }

    if (m_pendingJobs.load(std::memory_order_acquire) != 0U) {
      return false;
    }

    const bool dispatchFailed =
        m_graphDispatchFailed.load(std::memory_order_acquire);
    reset_graph_state_locked();
    m_graphActive = false;
    return !dispatchFailed;
  }

  JobHandle submit_job(Job job) noexcept {
    JobHandle handle{};

    if ((job.function == nullptr) || !is_initialized()) {
      return handle;
    }

    std::lock_guard<std::mutex> lock(m_graphMutex);

    if (!m_graphActive || m_graphDispatched || (m_nodeCount >= kMaxJobs)) {
      return handle;
    }

    const std::uint32_t nodeIndex = static_cast<std::uint32_t>(m_nodeCount);
    ++m_nodeCount;

    JobNode &node = m_nodes[nodeIndex];
    node.job = job;
    node.remainingDependencies.store(0U, std::memory_order_release);
    node.completed.store(false, std::memory_order_release);
    node.generation = m_generation;
    node.firstDependentEdge = kInvalidIndex;
    node.active = true;

    handle.id = encode_handle_id(nodeIndex, node.generation);
    return handle;
  }

  bool add_dependency(JobHandle prerequisite, JobHandle dependent) noexcept {
    if (!is_initialized()) {
      return false;
    }

    std::lock_guard<std::mutex> lock(m_graphMutex);

    if (!m_graphActive || m_graphDispatched) {
      return false;
    }

    if (!is_valid_handle_locked(prerequisite) ||
        !is_valid_handle_locked(dependent)) {
      return false;
    }

    const std::uint32_t prerequisiteIndex = decode_handle_index(prerequisite);
    const std::uint32_t dependentIndex = decode_handle_index(dependent);
    if (prerequisiteIndex == dependentIndex) {
      return false;
    }

    if (would_create_cycle_locked(prerequisiteIndex, dependentIndex)) {
      return false;
    }

    if (m_edgeCount >= kMaxEdges) {
      return false;
    }

    const std::uint32_t edgeIndex = static_cast<std::uint32_t>(m_edgeCount);
    ++m_edgeCount;

    DependencyEdge &edge = m_edges[edgeIndex];
    edge.dependentIndex = dependentIndex;
    edge.nextEdge = m_nodes[prerequisiteIndex].firstDependentEdge;
    m_nodes[prerequisiteIndex].firstDependentEdge = edgeIndex;

    m_nodes[dependentIndex].remainingDependencies.fetch_add(
        1U, std::memory_order_acq_rel);
    return true;
  }

  void wait_for_handle(JobHandle handle) noexcept {
    if (!is_initialized()) {
      return;
    }

    g_threadIndex = 0U;

    std::uint32_t nodeIndex = kInvalidIndex;

    {
      std::lock_guard<std::mutex> lock(m_graphMutex);

      if (!m_graphActive || !is_valid_handle_locked(handle)) {
        return;
      }

      nodeIndex = decode_handle_index(handle);

      if (!m_graphDispatched) {
        dispatch_graph_locked();
      }
    }

    while (true) {
      if (m_graphDispatchFailed.load(std::memory_order_acquire)) {
        break;
      }

      const bool handleCompleted = is_completed_fast(nodeIndex);
      const bool noPendingJobs =
          (m_pendingJobs.load(std::memory_order_acquire) == 0U);
      if (handleCompleted && noPendingJobs) {
        break;
      }

      std::uint32_t readyNodeIndex = kInvalidIndex;
      if (pop_ready_job(&readyNodeIndex)) {
        execute_job(readyNodeIndex);
        continue;
      }

      // Timed wait (not predicate wait): this thread must also wake to steal
      // newly readied jobs, which signal m_workAvailable rather than
      // m_completed — and with zero workers it is the only executor.
      std::unique_lock<std::mutex> lock(m_completionMutex);
      m_completed.wait_for(lock, std::chrono::milliseconds(1));
    }
  }

  bool is_valid_handle(JobHandle handle) noexcept {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    return is_valid_handle_locked(handle);
  }

  bool is_completed(JobHandle handle) noexcept {
    std::lock_guard<std::mutex> lock(m_graphMutex);

    if (!is_valid_handle_locked(handle)) {
      return false;
    }

    const std::uint32_t index = decode_handle_index(handle);
    return m_nodes[index].completed.load(std::memory_order_acquire);
  }

  std::uint32_t worker_count() const noexcept { return m_workerCount; }

  std::uint32_t thread_count() const noexcept { return m_workerCount + 1U; }

  JobSystemStats consume_stats() noexcept {
    JobSystemStats stats{};

    for (std::size_t i = 0U; i < m_threadStats.size(); ++i) {
      stats.jobsExecuted +=
          m_threadStats[i].jobsExecuted.exchange(0U, std::memory_order_acq_rel);
      stats.busyNanoseconds += m_threadStats[i].busyNanoseconds.exchange(
          0U, std::memory_order_acq_rel);
    }

    stats.queueContentionCount =
        m_queueContentionCount.exchange(0U, std::memory_order_acq_rel);
    return stats;
  }

private:
  void wait_for_all_jobs() noexcept {
    {
      std::lock_guard<std::mutex> lock(m_graphMutex);
      if (m_graphActive && !m_graphDispatched && (m_nodeCount > 0U)) {
        dispatch_graph_locked();
      }
    }

    g_threadIndex = 0U;
    while (m_pendingJobs.load(std::memory_order_acquire) != 0U) {
      std::uint32_t nodeIndex = kInvalidIndex;
      if (pop_ready_job(&nodeIndex)) {
        execute_job(nodeIndex);
        continue;
      }

      std::unique_lock<std::mutex> lock(m_completionMutex);
      m_completed.wait_for(lock, std::chrono::milliseconds(1));
    }
  }

  bool is_valid_handle_locked(JobHandle handle) const noexcept {
    if (handle.id == 0U) {
      return false;
    }

    const std::uint32_t index = decode_handle_index(handle);
    if (index >= m_nodeCount) {
      return false;
    }

    const JobNode &node = m_nodes[index];
    return node.active && (node.generation == decode_handle_generation(handle));
  }

  bool would_create_cycle_locked(std::uint32_t prerequisiteIndex,
                                 std::uint32_t dependentIndex) const noexcept {
    if (prerequisiteIndex == dependentIndex) {
      return true;
    }

    // Thread-local storage avoids ~40 KB of stack allocation per call.
    // visited is reset at the start of every search; stack elements are
    // always written before being read so no explicit reset is needed.
    thread_local static std::array<bool, kMaxJobs> visited{};
    thread_local static std::array<std::uint32_t, kMaxJobs> stack{};
    visited.fill(false);
    std::size_t stackCount = 0U;

    visited[dependentIndex] = true;
    stack[stackCount] = dependentIndex;
    ++stackCount;

    while (stackCount > 0U) {
      --stackCount;
      const std::uint32_t nodeIndex = stack[stackCount];
      if (nodeIndex == prerequisiteIndex) {
        return true;
      }

      std::uint32_t edgeIndex = m_nodes[nodeIndex].firstDependentEdge;
      while (edgeIndex != kInvalidIndex) {
        const std::uint32_t nextNodeIndex = m_edges[edgeIndex].dependentIndex;
        if (!visited[nextNodeIndex]) {
          visited[nextNodeIndex] = true;
          stack[stackCount] = nextNodeIndex;
          ++stackCount;
        }

        edgeIndex = m_edges[edgeIndex].nextEdge;
      }
    }

    return false;
  }

  void dispatch_graph_locked() noexcept {
    if (!m_graphActive || m_graphDispatched) {
      return;
    }

    const bool graphAcyclic = validate_graph_acyclic();
    if (!graphAcyclic) {
#ifndef NDEBUG
      assert(false && "job graph contains a cycle");
#endif
      m_graphDispatchFailed.store(true, std::memory_order_release);
      m_graphDispatched = true;
      m_pendingJobs.store(0U, std::memory_order_release);
      m_workAvailable.notify_all();
      m_completed.notify_all();
      return;
    }

    m_graphDispatched = true;
    m_pendingJobs.store(0U, std::memory_order_release);

    for (std::size_t i = 0U; i < m_nodeCount; ++i) {
      if (!m_nodes[i].active) {
        continue;
      }

      m_pendingJobs.fetch_add(1U, std::memory_order_acq_rel);
      if (m_nodes[i].remainingDependencies.load(std::memory_order_acquire) ==
          0U) {
        static_cast<void>(push_ready_job(static_cast<std::uint32_t>(i)));
      }
    }

    m_workAvailable.notify_all();
  }

  void reset_graph_state_locked() noexcept {
    for (std::size_t i = 0U; i < m_nodeCount; ++i) {
      m_nodes[i].active = false;
      m_nodes[i].completed.store(false, std::memory_order_release);
    }

    m_nodeCount = 0U;
    m_edgeCount = 0U;
    m_graphDispatched = false;
    m_graphDispatchFailed.store(false, std::memory_order_release);

    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_queueHead = 0U;
      m_queueCount = 0U;
    }

    m_pendingJobs.store(0U, std::memory_order_release);

    // Wrap within the handle-encodable width (skipping 0) so stored and
    // decoded generations always compare equal for live handles.
    m_generation = (m_generation + 1U) & kGenerationMask;
    if (m_generation == 0U) {
      m_generation = 1U;
    }
  }

  bool push_ready_job(std::uint32_t nodeIndex) noexcept {
    std::unique_lock<std::mutex> lock(m_queueMutex, std::defer_lock);
    if (!lock.try_lock()) {
      m_queueContentionCount.fetch_add(1U, std::memory_order_relaxed);
      lock.lock();
    }

    if (m_queueCount >= m_readyQueue.size()) {
      return false;
    }

    const std::size_t tail = (m_queueHead + m_queueCount) % m_readyQueue.size();
    m_readyQueue[tail] = nodeIndex;
    ++m_queueCount;
    return true;
  }

  bool pop_ready_job(std::uint32_t *outNodeIndex) noexcept {
    if (outNodeIndex == nullptr) {
      return false;
    }

    std::unique_lock<std::mutex> lock(m_queueMutex, std::defer_lock);
    if (!lock.try_lock()) {
      m_queueContentionCount.fetch_add(1U, std::memory_order_relaxed);
      lock.lock();
    }

    if (m_queueCount == 0U) {
      return false;
    }

    *outNodeIndex = m_readyQueue[m_queueHead];
    m_queueHead = (m_queueHead + 1U) % m_readyQueue.size();
    --m_queueCount;
    return true;
  }

  bool is_completed_fast(std::uint32_t nodeIndex) const noexcept {
    if (nodeIndex == kInvalidIndex) {
      return false;
    }

    return m_nodes[nodeIndex].completed.load(std::memory_order_acquire);
  }

  bool validate_graph_acyclic() const noexcept {
    thread_local static std::array<std::uint32_t, kMaxJobs> indegree{};
    thread_local static std::array<std::uint32_t, kMaxJobs> queue{};
    std::fill(indegree.begin(), indegree.begin() + m_nodeCount, 0U);
    std::size_t activeCount = 0U;

    for (std::size_t i = 0U; i < m_nodeCount; ++i) {
      if (!m_nodes[i].active) {
        continue;
      }

      indegree[i] =
          m_nodes[i].remainingDependencies.load(std::memory_order_acquire);
      ++activeCount;
    }

    std::size_t queueHead = 0U;
    std::size_t queueCount = 0U;
    for (std::size_t i = 0U; i < m_nodeCount; ++i) {
      if (!m_nodes[i].active) {
        continue;
      }

      if (indegree[i] == 0U) {
        queue[queueCount] = static_cast<std::uint32_t>(i);
        ++queueCount;
      }
    }

    std::size_t visitedCount = 0U;
    while (queueHead < queueCount) {
      const std::uint32_t nodeIndex = queue[queueHead];
      ++queueHead;
      ++visitedCount;

      std::uint32_t edgeIndex = m_nodes[nodeIndex].firstDependentEdge;
      while (edgeIndex != kInvalidIndex) {
        const std::uint32_t dependentIndex = m_edges[edgeIndex].dependentIndex;
        if (indegree[dependentIndex] > 0U) {
          --indegree[dependentIndex];
          if (indegree[dependentIndex] == 0U) {
            queue[queueCount] = dependentIndex;
            ++queueCount;
          }
        }

        edgeIndex = m_edges[edgeIndex].nextEdge;
      }
    }

    return visitedCount == activeCount;
  }

  void execute_job(std::uint32_t nodeIndex) noexcept {
    JobNode &node = m_nodes[nodeIndex];

    const auto start = std::chrono::steady_clock::now();
    node.job.function(node.job.data);
    const auto end = std::chrono::steady_clock::now();

    const auto busyNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start)
            .count());

    std::uint32_t threadIndex = current_thread_index();
    if (threadIndex >= m_threadStats.size()) {
      threadIndex = 0U;
    }

    m_threadStats[threadIndex].jobsExecuted.fetch_add(
        1U, std::memory_order_relaxed);
    m_threadStats[threadIndex].busyNanoseconds.fetch_add(
        busyNs, std::memory_order_relaxed);

    std::uint32_t edgeIndex = node.firstDependentEdge;
    while (edgeIndex != kInvalidIndex) {
      const std::uint32_t dependentIndex = m_edges[edgeIndex].dependentIndex;
      JobNode &dependentNode = m_nodes[dependentIndex];

      if (dependentNode.remainingDependencies.fetch_sub(
              1U, std::memory_order_acq_rel) == 1U) {
        static_cast<void>(push_ready_job(dependentIndex));
        m_workAvailable.notify_one();
      }

      edgeIndex = m_edges[edgeIndex].nextEdge;
    }

    const bool wasLastJob =
        (m_pendingJobs.fetch_sub(1U, std::memory_order_acq_rel) == 1U);

    node.completed.store(true, std::memory_order_release);

    if (wasLastJob) {
      std::lock_guard<std::mutex> lock(m_completionMutex);
      m_completed.notify_all();
    }
  }

  void worker_loop(std::uint32_t threadIndex) noexcept {
    g_threadIndex = threadIndex;

    // Event-driven: sleep until work is pushed or shutdown begins. The wait
    // predicate re-checks queue state under m_queueMutex — the same mutex
    // every push holds — so wakeups cannot be lost.
    for (;;) {
      std::uint32_t nodeIndex = kInvalidIndex;
      {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_workAvailable.wait(lock, [this]() noexcept {
          return (m_queueCount > 0U) ||
                 !m_running.load(std::memory_order_acquire);
        });

        if (m_queueCount == 0U) {
          return; // Shutdown with no work left.
        }

        nodeIndex = m_readyQueue[m_queueHead];
        m_queueHead = (m_queueHead + 1U) % m_readyQueue.size();
        --m_queueCount;
      }

      execute_job(nodeIndex);
    }
  }

  std::array<std::thread, kMaxWorkers> m_workers{};
  std::array<JobNode, kMaxJobs> m_nodes{};
  std::array<DependencyEdge, kMaxEdges> m_edges{};
  std::array<std::uint32_t, kMaxJobs> m_readyQueue{};
  std::array<ThreadStats, kMaxWorkers + 1U> m_threadStats{};

  std::atomic<bool> m_initialized = false;
  std::atomic<bool> m_running = false;
  std::atomic<bool> m_graphDispatchFailed = false;
  std::atomic<std::uint64_t> m_pendingJobs = 0U;
  std::atomic<std::uint64_t> m_queueContentionCount = 0U;

  std::uint32_t m_generation = 1U;
  std::uint32_t m_workerCount = 0U;
  std::size_t m_nodeCount = 0U;
  std::size_t m_edgeCount = 0U;
  std::size_t m_queueHead = 0U;
  std::size_t m_queueCount = 0U;
  bool m_graphActive = false;
  bool m_graphDispatched = false;

  std::mutex m_graphMutex;
  // Lock order: m_graphMutex -> m_queueMutex. Workers block on m_queueMutex
  // via m_workAvailable; pushes hold it, so predicate checks are race-free.
  std::mutex m_queueMutex;
  std::mutex m_completionMutex;
  std::condition_variable m_workAvailable;
  std::condition_variable m_completed;
};

JobSystem g_jobSystem;

} // namespace

/// Initializes the owning system for job system.
bool initialize_job_system(std::uint32_t workerCount) noexcept {
  return g_jobSystem.initialize(workerCount);
}

/// Shuts down the owning system for job system.
void shutdown_job_system() noexcept { g_jobSystem.shutdown(); }

/// Returns whether is job system initialized.
bool is_job_system_initialized() noexcept {
  return g_jobSystem.is_initialized();
}

/// Begins the requested operation or profiling range for frame graph.
bool begin_frame_graph() noexcept { return g_jobSystem.begin_graph(); }

/// Ends the requested operation or profiling range for frame graph.
bool end_frame_graph() noexcept { return g_jobSystem.end_graph(); }

/// Submits work to the owning buffer or system.
JobHandle submit(Job job) noexcept { return g_jobSystem.submit_job(job); }

/// Adds a value or component to the target system for dependency.
bool add_dependency(JobHandle prerequisite, JobHandle dependent) noexcept {
  return g_jobSystem.add_dependency(prerequisite, dependent);
}

/// Handles wait.
void wait(JobHandle handle) noexcept { g_jobSystem.wait_for_handle(handle); }

/// Returns whether is valid handle.
bool is_valid_handle(JobHandle handle) noexcept {
  return g_jobSystem.is_valid_handle(handle);
}

/// Returns whether is completed.
bool is_completed(JobHandle handle) noexcept {
  return g_jobSystem.is_completed(handle);
}

/// Handles worker count.
std::uint32_t worker_count() noexcept { return g_jobSystem.worker_count(); }

/// Handles thread count.
std::uint32_t thread_count() noexcept { return g_jobSystem.thread_count(); }

/// Handles current thread index.
std::uint32_t current_thread_index() noexcept { return g_threadIndex; }

/// Handles consume job stats.
JobSystemStats consume_job_stats() noexcept {
  return g_jobSystem.consume_stats();
}

} // namespace engine::core
