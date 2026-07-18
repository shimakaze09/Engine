// Declares asset streaming types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "engine/renderer/asset_database.h"

namespace engine::renderer {

/// Priority levels for async load requests (highest = Immediate).
enum class LoadPriority : std::uint8_t {
  Low = 0,
  Normal = 1,
  High = 2,
  Immediate = 3
};

/// Loading states tracked per request.
enum class LoadingState : std::uint8_t {
  Queued = 0,
  Loading = 1,
  Uploading = 2,
  Ready = 3,
  Failed = 4
};

/// Opaque handle returned by load_async.  Users poll with is_ready() or
/// block with wait().
struct LoadHandle final {
  std::uint32_t index = 0xFFFFFFFFU;
  std::uint32_t generation = 0U;

  static constexpr std::uint32_t kInvalid = 0xFFFFFFFFU;

  /// True for handles returned by a successful load_asset_async.
  [[nodiscard]] bool valid() const noexcept { return index != kInvalid; }

  friend constexpr bool operator==(const LoadHandle &,
                                   const LoadHandle &) = default;
};

inline constexpr LoadHandle kInvalidLoadHandle{};

/// Performs CPU-side asset loading for a streaming request.
using AssetLoadCallback = bool (*)(AssetId id, const char *path,
                                   std::uint64_t *outSizeBytes,
                                   void *userData) noexcept;

/// Performs main-thread upload for a loaded streaming request.
using AssetUploadCallback = bool (*)(AssetId id, void *userData) noexcept;

/// Per-request payload tracked inside the streaming queue.
struct LoadRequest final {
  AssetId assetId = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
  LoadPriority priority = LoadPriority::Normal;
  LoadingState state = LoadingState::Queued;
  std::uint64_t loadedSizeBytes = 0ULL;
  std::uint32_t generation = 1U;
  bool loadInProgress = false;
  bool occupied = false;
};

/// Fixed-capacity streaming queue processed by a background IO worker pool.
struct AssetStreamingQueue final {
  AssetStreamingQueue() noexcept = default;
  ~AssetStreamingQueue() noexcept;

  AssetStreamingQueue(const AssetStreamingQueue &) = delete;
  AssetStreamingQueue &operator=(const AssetStreamingQueue &) = delete;

  static constexpr std::size_t kMaxRequests = 1024U;
  static constexpr std::size_t kWorkerCount = 4U;
  LoadRequest requests[kMaxRequests]{};
  std::size_t count = 0U;

  // Budget CVars are read each frame:
  std::uint64_t streamingBudgetBytes = 256ULL * 1024ULL * 1024ULL; // 256 MB
  std::uint32_t maxUploadsPerFrame = 8U;

  // Frame-local tracking:
  std::uint64_t inflight_bytes_this_frame = 0ULL;
  std::uint32_t uploads_this_frame = 0U;

  AssetLoadCallback loadCallback = nullptr;
  AssetUploadCallback uploadCallback = nullptr;
  void *callbackUserData = nullptr;

  mutable std::mutex mutex{};
  mutable std::condition_variable stateChanged{};
  std::array<std::thread, kWorkerCount> workerThreads{};
  bool workerRunning = false;
  bool workerStopRequested = false;
};

// ---- Lifecycle ----

/// Initialise the streaming queue. Registers CVars.
bool initialize_asset_streaming(AssetStreamingQueue *queue) noexcept;

/// Shutdown and drain all pending requests.
void shutdown_asset_streaming(AssetStreamingQueue *queue) noexcept;

// ---- Request management ----

/// Queue an async load.  Returns a LoadHandle for polling.
LoadHandle load_asset_async(AssetStreamingQueue *queue, AssetId id,
                            const char *sourcePath,
                            LoadPriority priority) noexcept;

/// Update the priority of a queued (not yet loading) request.
bool update_load_priority(AssetStreamingQueue *queue, LoadHandle handle,
                          LoadPriority newPriority) noexcept;

/// Cancel a pending request.
bool cancel_load(AssetStreamingQueue *queue, LoadHandle handle) noexcept;

/// Release a completed or failed request slot after the caller has observed its
/// terminal state.
bool release_load(AssetStreamingQueue *queue, LoadHandle handle) noexcept;

// ---- Polling ----

/// Check if the load has reached the Ready state.
bool is_load_ready(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept;

/// Get current loading state.
LoadingState get_load_state(const AssetStreamingQueue *queue,
                            LoadHandle handle) noexcept;

/// Blocking wait until the request reaches Ready or Failed.
/// Uses the queue condition variable; callers must still drive
/// update_asset_streaming so loaded requests can be uploaded on the main thread.
void wait_for_load(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept;

// ---- Per-frame processing ----

/// Called once per frame on the main thread. Performs GPU uploads from
/// Uploading to Ready up to maxUploadsPerFrame, then schedules the
/// highest-priority queued requests while the worker pool and frame budget
/// allow more work.
///
/// @param loadCallback   Called for each request that needs file IO.  Should
///                       read the asset file into CPU memory and return true
///                       on success.  May run concurrently on streaming worker
///                       threads.  May be nullptr for budget-only tests.
/// @param uploadCallback Called for each request that needs GPU upload.  Should
///                       upload to GL and return true on success.  May be
///                       nullptr for budget-only tests.
/// @param userData       Forwarded to both callbacks. Must be safe for
///                       concurrent load callbacks when loadCallback is used.
/// @return Number of requests that transitioned to Ready this frame.
std::size_t update_asset_streaming(
    AssetStreamingQueue *queue,
    AssetLoadCallback loadCallback,
    AssetUploadCallback uploadCallback,
    void *userData) noexcept;

/// Reset per-frame counters. Call at the start of each frame.
void begin_streaming_frame(AssetStreamingQueue *queue) noexcept;

/// Query how many requests are currently queued or in-flight.
std::size_t pending_load_count(const AssetStreamingQueue *queue) noexcept;

} // namespace engine::renderer
