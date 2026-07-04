// Implements asset streaming behavior for the Engine renderer system.

#include "engine/renderer/asset_streaming.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"

namespace engine::renderer {

namespace {

constexpr std::uint64_t kUnknownLoadBudgetBytes = 64ULL * 1024ULL * 1024ULL;

/// Writes path data.
void write_path(std::array<char, 260U> *out, const char *src) noexcept {
  out->fill('\0');
  if (src == nullptr) {
    return;
  }
  const std::size_t len = std::strlen(src);
  const std::size_t maxCopy = out->size() - 1U;
  const std::size_t copyLen = (len > maxCopy) ? maxCopy : len;
  if (copyLen > 0U) {
    std::memcpy(out->data(), src, copyLen);
  }
  (*out)[copyLen] = '\0';
}

/// Finds the matching object or resource for free request.
std::uint32_t find_free_request(const AssetStreamingQueue *queue) noexcept {
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    if (!queue->requests[i].occupied) {
      return i;
    }
  }
  return LoadHandle::kInvalid;
}

/// Returns the next non-zero generation for a reused request slot.
std::uint32_t next_generation(std::uint32_t generation) noexcept {
  ++generation;
  return generation == 0U ? 1U : generation;
}

/// Clears one request slot while invalidating handles that reference it.
void reset_request_unlocked(AssetStreamingQueue *queue,
                            std::uint32_t index) noexcept {
  const std::uint32_t generation =
      next_generation(queue->requests[index].generation);
  queue->requests[index] = LoadRequest{};
  queue->requests[index].generation = generation;
}

/// Checks whether a handle still references the current request generation.
bool is_current_handle_unlocked(const AssetStreamingQueue *queue,
                                LoadHandle handle) noexcept {
  return handle.valid() && (handle.index < AssetStreamingQueue::kMaxRequests) &&
         (queue->requests[handle.index].generation == handle.generation);
}

/// Sort-stable selection of the highest-priority Queued request.
std::uint32_t
pick_highest_priority_queued(const AssetStreamingQueue *queue) noexcept {
  std::uint32_t best = LoadHandle::kInvalid;
  auto bestPri = LoadPriority::Low;

  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    const LoadRequest &req = queue->requests[i];
    if (req.occupied && (req.state == LoadingState::Queued)) {
      if ((best == LoadHandle::kInvalid) ||
          (static_cast<std::uint8_t>(req.priority) >
           static_cast<std::uint8_t>(bestPri))) {
        best = i;
        bestPri = req.priority;
      }
    }
  }
  return best;
}

/// Counts load jobs already assigned to the worker pool.
std::size_t active_load_count_unlocked(
    const AssetStreamingQueue *queue) noexcept {
  std::size_t count = 0U;
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    const LoadRequest &req = queue->requests[i];
    if (req.occupied &&
        ((req.state == LoadingState::Loading) || req.loadInProgress)) {
      ++count;
    }
  }
  return count;
}

/// Counts loaded bytes waiting for main-thread upload.
std::uint64_t pending_upload_bytes_unlocked(
    const AssetStreamingQueue *queue) noexcept {
  std::uint64_t bytes = 0ULL;
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    const LoadRequest &req = queue->requests[i];
    if (req.occupied && (req.state == LoadingState::Uploading)) {
      bytes += req.loadedSizeBytes;
    }
  }
  return bytes;
}

/// Computes how many worker loads can be admitted before their sizes are known.
std::size_t budgeted_load_slots(std::uint64_t streamingBudgetBytes,
                                std::uint64_t pendingUploadBytes) noexcept {
  if (pendingUploadBytes >= streamingBudgetBytes) {
    return 0U;
  }

  const std::uint64_t availableBytes = streamingBudgetBytes - pendingUploadBytes;
  std::size_t slots =
      static_cast<std::size_t>(availableBytes / kUnknownLoadBudgetBytes);
  if (slots == 0U) {
    slots = 1U;
  }
  if (slots > AssetStreamingQueue::kWorkerCount) {
    slots = AssetStreamingQueue::kWorkerCount;
  }
  return slots;
}

/// Resets queue request state while the queue mutex is already held.
void reset_requests_unlocked(AssetStreamingQueue *queue) noexcept {
  for (std::size_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    reset_request_unlocked(queue, static_cast<std::uint32_t>(i));
  }
  queue->count = 0U;
  queue->inflight_bytes_this_frame = 0ULL;
  queue->uploads_this_frame = 0U;
  queue->loadCallback = nullptr;
  queue->uploadCallback = nullptr;
  queue->callbackUserData = nullptr;
}

/// Finds a queued worker job while the queue mutex is already held.
std::uint32_t find_worker_job_unlocked(const AssetStreamingQueue *queue)
    noexcept {
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    const LoadRequest &req = queue->requests[i];
    if (req.occupied && (req.state == LoadingState::Loading) &&
        !req.loadInProgress) {
      return i;
    }
  }
  return LoadHandle::kInvalid;
}

/// Runs CPU-side load callbacks on the streaming worker thread.
void streaming_worker_main(AssetStreamingQueue *queue) noexcept {
  while (true) {
    std::uint32_t index = LoadHandle::kInvalid;
    AssetId assetId = kInvalidAssetId;
    char sourcePath[260]{};
    AssetLoadCallback loadCallback = nullptr;
    void *userData = nullptr;

    {
      std::unique_lock<std::mutex> lock(queue->mutex);
      queue->stateChanged.wait(lock, [&]() noexcept {
        return queue->workerStopRequested ||
               (find_worker_job_unlocked(queue) != LoadHandle::kInvalid);
      });

      if (queue->workerStopRequested) {
        return;
      }

      index = find_worker_job_unlocked(queue);
      if (index == LoadHandle::kInvalid) {
        continue;
      }

      LoadRequest &request = queue->requests[index];
      request.loadInProgress = true;
      assetId = request.assetId;
      std::memcpy(sourcePath, request.sourcePath.data(),
                  request.sourcePath.size());
      sourcePath[sizeof(sourcePath) - 1U] = '\0';
      loadCallback = queue->loadCallback;
      userData = queue->callbackUserData;
    }

    std::uint64_t sizeBytes = 0ULL;
    const bool loaded =
        (loadCallback != nullptr)
            ? loadCallback(assetId, sourcePath, &sizeBytes, userData)
            : true;

    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      if (index < AssetStreamingQueue::kMaxRequests) {
        LoadRequest &request = queue->requests[index];
        if (request.occupied && request.loadInProgress &&
            (request.assetId == assetId) &&
            (request.state == LoadingState::Loading)) {
          request.loadInProgress = false;
          request.loadedSizeBytes = sizeBytes;
          request.state = loaded ? LoadingState::Uploading
                                 : LoadingState::Failed;
          if (!loaded) {
            core::log_message(core::LogLevel::Error, "streaming",
                              "load callback failed for asset");
          }
        }
      }
    }
    queue->stateChanged.notify_all();
  }
}

} // namespace

// ---- Lifecycle ----

AssetStreamingQueue::~AssetStreamingQueue() noexcept {
  shutdown_asset_streaming(this);
}

bool initialize_asset_streaming(AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return false;
  }

  // Register CVars for streaming budget.
  engine::core::cvar_register_int("asset.streaming_budget_mb", 256,
                                  "Max memory (MB) for in-flight asset loads");
  engine::core::cvar_register_int("asset.max_uploads_per_frame", 8,
                                  "Max GPU uploads per frame");
  engine::core::cvar_register_int(
      "asset.cache_size_mb", 512,
      "Max cached asset memory (MB) before LRU eviction");

  shutdown_asset_streaming(queue);
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    reset_requests_unlocked(queue);
    queue->workerStopRequested = false;
    queue->workerRunning = true;
  }

  for (std::thread &workerThread : queue->workerThreads) {
    workerThread = std::thread(streaming_worker_main, queue);
  }
  return true;
}

/// Shuts down the owning system for asset streaming.
void shutdown_asset_streaming(AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->workerStopRequested = true;
  }
  queue->stateChanged.notify_all();

  for (std::thread &workerThread : queue->workerThreads) {
    if (workerThread.joinable()) {
      workerThread.join();
    }
  }

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    reset_requests_unlocked(queue);
    queue->workerRunning = false;
    queue->workerStopRequested = false;
  }
}

// ---- Request management ----

LoadHandle load_asset_async(AssetStreamingQueue *queue, AssetId id,
                            const char *sourcePath,
                            LoadPriority priority) noexcept {
  if ((queue == nullptr) || (id == kInvalidAssetId)) {
    core::log_message(core::LogLevel::Error, "streaming",
                      "load_asset_async: null queue or invalid id");
    return kInvalidLoadHandle;
  }

  std::lock_guard<std::mutex> lock(queue->mutex);

  // Check if already queued.
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    if (queue->requests[i].occupied && (queue->requests[i].assetId == id) &&
        (queue->requests[i].state != LoadingState::Failed)) {
      // Already in flight — update priority if higher.
      if (static_cast<std::uint8_t>(priority) >
          static_cast<std::uint8_t>(queue->requests[i].priority)) {
        queue->requests[i].priority = priority;
      }
      return LoadHandle{i, queue->requests[i].generation};
    }
  }

  const std::uint32_t slot = find_free_request(queue);
  if (slot == LoadHandle::kInvalid) {
    core::log_message(core::LogLevel::Error, "streaming",
                      "load_asset_async: queue full");
    return kInvalidLoadHandle;
  }

  LoadRequest &req = queue->requests[slot];
  req.assetId = id;
  write_path(&req.sourcePath, sourcePath);
  req.priority = priority;
  req.state = LoadingState::Queued;
  req.occupied = true;
  ++queue->count;
  queue->stateChanged.notify_all();

  return LoadHandle{slot, req.generation};
}

/// Advances this system for the current frame or tick for load priority.
bool update_load_priority(AssetStreamingQueue *queue, LoadHandle handle,
                          LoadPriority newPriority) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(queue->mutex);
  if (!is_current_handle_unlocked(queue, handle)) {
    return false;
  }
  LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied || (req.state != LoadingState::Queued)) {
    return false; // Can only update priority while still queued.
  }

  req.priority = newPriority;
  return true;
}

/// Handles cancel load.
bool cancel_load(AssetStreamingQueue *queue, LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(queue->mutex);
  if (!is_current_handle_unlocked(queue, handle)) {
    return false;
  }
  LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied) {
    return false;
  }
  if (req.loadInProgress) {
    return false;
  }

  reset_request_unlocked(queue, handle.index);
  if (queue->count > 0U) {
    --queue->count;
  }
  queue->stateChanged.notify_all();
  return true;
}

bool release_load(AssetStreamingQueue *queue, LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(queue->mutex);
  if (!is_current_handle_unlocked(queue, handle)) {
    return false;
  }
  LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied) {
    return false;
  }
  if ((req.state != LoadingState::Ready) && (req.state != LoadingState::Failed)) {
    return false;
  }

  reset_request_unlocked(queue, handle.index);
  if (queue->count > 0U) {
    --queue->count;
  }
  queue->stateChanged.notify_all();
  return true;
}

// ---- Polling ----

bool is_load_ready(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue->mutex);
  if (!is_current_handle_unlocked(queue, handle)) {
    return false;
  }
  const LoadRequest &req = queue->requests[handle.index];
  return req.occupied && (req.state == LoadingState::Ready);
}

/// Returns the requested value for load state.
LoadingState get_load_state(const AssetStreamingQueue *queue,
                            LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return LoadingState::Failed;
  }
  std::lock_guard<std::mutex> lock(queue->mutex);
  if (!is_current_handle_unlocked(queue, handle)) {
    return LoadingState::Failed;
  }
  const LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied) {
    return LoadingState::Failed;
  }
  return req.state;
}

/// Handles wait for load.
void wait_for_load(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return;
  }

  std::unique_lock<std::mutex> lock(queue->mutex);
  if (!is_current_handle_unlocked(queue, handle)) {
    return;
  }
  queue->stateChanged.wait(lock, [&]() noexcept {
    if (!is_current_handle_unlocked(queue, handle)) {
      return true;
    }
    const LoadRequest &request = queue->requests[handle.index];
    return !request.occupied || (request.state == LoadingState::Ready) ||
           (request.state == LoadingState::Failed);
  });
}

// ---- Per-frame processing ----

void begin_streaming_frame(AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(queue->mutex);

  // Refresh budget from CVars.
  const int budgetMb =
      engine::core::cvar_get_int("asset.streaming_budget_mb", 256);
  queue->streamingBudgetBytes =
      static_cast<std::uint64_t>(budgetMb > 0 ? budgetMb : 256) * 1024ULL *
      1024ULL;
  const int uploadsPerFrame =
      engine::core::cvar_get_int("asset.max_uploads_per_frame", 8);
  queue->maxUploadsPerFrame =
      uploadsPerFrame > 0 ? static_cast<std::uint32_t>(uploadsPerFrame) : 8U;

  queue->inflight_bytes_this_frame = 0ULL;
  queue->uploads_this_frame = 0U;
}

/// Advances this system for the current frame or tick for asset streaming.
std::size_t update_asset_streaming(
    AssetStreamingQueue *queue,
    AssetLoadCallback loadCallback,
    AssetUploadCallback uploadCallback,
    void *userData) noexcept {
  if (queue == nullptr) {
    return 0U;
  }

  std::size_t readyCount = 0U;

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->loadCallback = loadCallback;
    queue->uploadCallback = uploadCallback;
    queue->callbackUserData = userData;
  }

  // Phase 1: Promote Uploading -> Ready up to maxUploadsPerFrame.
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    AssetId assetId = kInvalidAssetId;
    AssetUploadCallback callback = nullptr;
    void *callbackUserData = nullptr;

    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      if (queue->uploads_this_frame >= queue->maxUploadsPerFrame) {
        break;
      }

      LoadRequest &req = queue->requests[i];
      if (!req.occupied || (req.state != LoadingState::Uploading)) {
        continue;
      }
      assetId = req.assetId;
      callback = queue->uploadCallback;
      callbackUserData = queue->callbackUserData;
    }

    const bool uploaded =
        (callback != nullptr) ? callback(assetId, callbackUserData) : true;

    {
      std::lock_guard<std::mutex> lock(queue->mutex);
      LoadRequest &req = queue->requests[i];
      if (!req.occupied || (req.assetId != assetId) ||
          (req.state != LoadingState::Uploading)) {
        continue;
      }

      if (uploaded) {
        req.state = LoadingState::Ready;
        queue->inflight_bytes_this_frame += req.loadedSizeBytes;
        ++queue->uploads_this_frame;
        ++readyCount;
      } else {
        core::log_message(core::LogLevel::Error, "streaming",
                          "upload callback failed for asset");
        req.state = LoadingState::Failed;
      }
    }
    queue->stateChanged.notify_all();
  }

  {
    std::lock_guard<std::mutex> lock(queue->mutex);

    // Phase 2: schedule queued requests for idle workers. The loaded byte size
    // is only known after CPU IO finishes, so the budget gate uses bytes that
    // are already loaded and waiting for upload.
    std::size_t activeLoads = active_load_count_unlocked(queue);
    const std::uint64_t pendingUploadBytes =
        pending_upload_bytes_unlocked(queue);
    const std::size_t budgetedLoads =
        budgeted_load_slots(queue->streamingBudgetBytes, pendingUploadBytes);
    bool scheduledAny = false;

    while (activeLoads < budgetedLoads) {
      const std::uint32_t idx = pick_highest_priority_queued(queue);
      if (idx == LoadHandle::kInvalid) {
        break;
      }

      LoadRequest &req = queue->requests[idx];
      req.state = LoadingState::Loading;
      ++activeLoads;
      scheduledAny = true;
    }

    if (scheduledAny) {
      queue->stateChanged.notify_all();
    }
  }

  // Completed/failed requests remain observable until callers retire their
  // terminal handles with release_load().

  return readyCount;
}

/// Handles pending load count.
std::size_t pending_load_count(const AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return 0U;
  }

  std::lock_guard<std::mutex> lock(queue->mutex);
  std::size_t pending = 0U;
  for (std::size_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    const LoadRequest &req = queue->requests[i];
    if (req.occupied && (req.state != LoadingState::Ready) &&
        (req.state != LoadingState::Failed)) {
      ++pending;
    }
  }
  return pending;
}

} // namespace engine::renderer
