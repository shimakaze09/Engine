#pragma once

#include <cstddef>
#include <cstdint>

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

  static constexpr std::uint32_t kInvalid = 0xFFFFFFFFU;

  [[nodiscard]] bool valid() const noexcept { return index != kInvalid; }

  friend constexpr bool operator==(const LoadHandle &,
                                   const LoadHandle &) = default;
};

inline constexpr LoadHandle kInvalidLoadHandle{};

/// Per-request payload tracked inside the streaming queue.
struct LoadRequest final {
  AssetId assetId = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
  LoadPriority priority = LoadPriority::Normal;
  LoadingState state = LoadingState::Queued;
  bool occupied = false;
};

/// Fixed-capacity streaming queue processed by a background IO thread.
struct AssetStreamingQueue final {
  static constexpr std::size_t kMaxRequests = 1024U;
  LoadRequest requests[kMaxRequests]{};
  std::size_t count = 0U;

  // Budget CVars are read each frame:
  std::uint64_t streamingBudgetBytes = 256ULL * 1024ULL * 1024ULL; // 256 MB
  std::uint32_t maxUploadsPerFrame = 8U;

  // Frame-local tracking:
  std::uint64_t inflight_bytes_this_frame = 0ULL;
  std::uint32_t uploads_this_frame = 0U;
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

// ---- Polling ----

/// Check if the load has reached the Ready state.
bool is_load_ready(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept;

/// Get current loading state.
LoadingState get_load_state(const AssetStreamingQueue *queue,
                            LoadHandle handle) noexcept;

/// Blocking wait until the request reaches Ready or Failed.
/// In a real engine this would use a condition variable; here we spin-poll
/// (acceptable for tooling / test use).
void wait_for_load(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept;

// ---- Per-frame processing ----

/// Called once per frame on the main thread.  Processes the highest-priority
/// queued requests up to the streaming budget, advances Loading→Uploading
/// for completed IO, and performs GPU uploads (Uploading→Ready) up to
/// maxUploadsPerFrame.
///
/// @param loadCallback   Called for each request that needs file IO.  Should
///                       read the asset file into CPU memory and return true
///                       on success.  May be nullptr for budget-only tests.
/// @param uploadCallback Called for each request that needs GPU upload.  Should
///                       upload to GL and return true on success.  May be
///                       nullptr for budget-only tests.
/// @param userData       Forwarded to both callbacks.
/// @return Number of requests that transitioned to Ready this frame.
std::size_t update_asset_streaming(
    AssetStreamingQueue *queue,
    bool (*loadCallback)(AssetId id, const char *path,
                         std::uint64_t *outSizeBytes, void *userData) noexcept,
    bool (*uploadCallback)(AssetId id, void *userData) noexcept,
    void *userData) noexcept;

/// Reset per-frame counters. Call at the start of each frame.
void begin_streaming_frame(AssetStreamingQueue *queue) noexcept;

/// Query how many requests are currently queued or in-flight.
std::size_t pending_load_count(const AssetStreamingQueue *queue) noexcept;

} // namespace engine::renderer
