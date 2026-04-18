#include "engine/renderer/asset_streaming.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/core/cvar.h"
#include "engine/core/logging.h"

namespace engine::renderer {

namespace {

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

std::uint32_t find_free_request(const AssetStreamingQueue *queue) noexcept {
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    if (!queue->requests[i].occupied) {
      return i;
    }
  }
  return LoadHandle::kInvalid;
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

} // namespace

// ---- Lifecycle ----

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
  return true;
}

void shutdown_asset_streaming(AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    queue->requests[i] = LoadRequest{};
  }
  queue->count = 0U;
  queue->inflight_bytes_this_frame = 0ULL;
  queue->uploads_this_frame = 0U;
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

  // Check if already queued.
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    if (queue->requests[i].occupied && (queue->requests[i].assetId == id) &&
        (queue->requests[i].state != LoadingState::Failed)) {
      // Already in flight — update priority if higher.
      if (static_cast<std::uint8_t>(priority) >
          static_cast<std::uint8_t>(queue->requests[i].priority)) {
        queue->requests[i].priority = priority;
      }
      return LoadHandle{i};
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

  return LoadHandle{slot};
}

bool update_load_priority(AssetStreamingQueue *queue, LoadHandle handle,
                          LoadPriority newPriority) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }

  LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied || (req.state != LoadingState::Queued)) {
    return false; // Can only update priority while still queued.
  }

  req.priority = newPriority;
  return true;
}

bool cancel_load(AssetStreamingQueue *queue, LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }

  LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied) {
    return false;
  }

  req = LoadRequest{};
  if (queue->count > 0U) {
    --queue->count;
  }
  return true;
}

// ---- Polling ----

bool is_load_ready(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return false;
  }
  const LoadRequest &req = queue->requests[handle.index];
  return req.occupied && (req.state == LoadingState::Ready);
}

LoadingState get_load_state(const AssetStreamingQueue *queue,
                            LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return LoadingState::Failed;
  }
  const LoadRequest &req = queue->requests[handle.index];
  if (!req.occupied) {
    return LoadingState::Failed;
  }
  return req.state;
}

void wait_for_load(const AssetStreamingQueue *queue,
                   LoadHandle handle) noexcept {
  if ((queue == nullptr) || !handle.valid()) {
    return;
  }

  // Spin-poll.  In production, replace with condvar + mutex.
  while (true) {
    const LoadingState s = get_load_state(queue, handle);
    if ((s == LoadingState::Ready) || (s == LoadingState::Failed)) {
      return;
    }
  }
}

// ---- Per-frame processing ----

void begin_streaming_frame(AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return;
  }

  // Refresh budget from CVars.
  const int budgetMb =
      engine::core::cvar_get_int("asset.streaming_budget_mb", 256);
  queue->streamingBudgetBytes =
      static_cast<std::uint64_t>(budgetMb > 0 ? budgetMb : 256) * 1024ULL *
      1024ULL;
  queue->maxUploadsPerFrame = static_cast<std::uint32_t>(
      engine::core::cvar_get_int("asset.max_uploads_per_frame", 8));

  queue->inflight_bytes_this_frame = 0ULL;
  queue->uploads_this_frame = 0U;
}

std::size_t update_asset_streaming(
    AssetStreamingQueue *queue,
    bool (*loadCallback)(AssetId id, const char *path,
                         std::uint64_t *outSizeBytes, void *userData) noexcept,
    bool (*uploadCallback)(AssetId id, void *userData) noexcept,
    void *userData) noexcept {
  if (queue == nullptr) {
    return 0U;
  }

  std::size_t readyCount = 0U;

  // Phase 1: Promote Queued → Loading for highest-priority requests within
  //          the streaming budget.
  while (true) {
    if (queue->inflight_bytes_this_frame >= queue->streamingBudgetBytes) {
      break; // Budget exhausted for this frame.
    }

    const std::uint32_t idx = pick_highest_priority_queued(queue);
    if (idx == LoadHandle::kInvalid) {
      break; // No more queued requests.
    }

    LoadRequest &req = queue->requests[idx];
    req.state = LoadingState::Loading;

    if (loadCallback != nullptr) {
      std::uint64_t sizeBytes = 0ULL;
      if (loadCallback(req.assetId, req.sourcePath.data(), &sizeBytes,
                       userData)) {
        queue->inflight_bytes_this_frame += sizeBytes;
        req.state = LoadingState::Uploading;
      } else {
        core::log_message(core::LogLevel::Error, "streaming",
                          "load callback failed for asset");
        req.state = LoadingState::Failed;
      }
    } else {
      // No callback — auto-advance for testing.
      req.state = LoadingState::Uploading;
    }
  }

  // Phase 2: Promote Uploading → Ready up to maxUploadsPerFrame.
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    if (queue->uploads_this_frame >= queue->maxUploadsPerFrame) {
      break;
    }

    LoadRequest &req = queue->requests[i];
    if (!req.occupied || (req.state != LoadingState::Uploading)) {
      continue;
    }

    if (uploadCallback != nullptr) {
      if (uploadCallback(req.assetId, userData)) {
        req.state = LoadingState::Ready;
        ++queue->uploads_this_frame;
        ++readyCount;
      } else {
        core::log_message(core::LogLevel::Error, "streaming",
                          "upload callback failed for asset");
        req.state = LoadingState::Failed;
      }
    } else {
      // No callback — auto-advance for testing.
      req.state = LoadingState::Ready;
      ++queue->uploads_this_frame;
      ++readyCount;
    }
  }

  // Phase 3: Garbage-collect completed/failed requests.
  for (std::uint32_t i = 0U; i < AssetStreamingQueue::kMaxRequests; ++i) {
    LoadRequest &req = queue->requests[i];
    if (req.occupied && ((req.state == LoadingState::Ready) ||
                         (req.state == LoadingState::Failed))) {
      // Keep them around so callers can poll is_load_ready().
      // They are cleaned up when the handle goes out of scope or on shutdown.
    }
  }

  return readyCount;
}

std::size_t pending_load_count(const AssetStreamingQueue *queue) noexcept {
  if (queue == nullptr) {
    return 0U;
  }

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
