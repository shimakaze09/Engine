// Implements the generic asset transition-request queue for the Engine
// content system (#171 C3: the ring and its push/pop/pending logic moved
// verbatim from the renderer's asset manager).

#include "engine/content/asset_request_queue.h"

#include "engine/core/logging.h"
#include "engine/core/string_util.h"

namespace engine::content {

namespace {

/// Copies a source path into a request field (zero-fills the tail so
/// requests stay byte-comparable).
void copy_source_path(std::array<char, 260U> *outPath,
                      const char *sourcePath) noexcept {
  if (outPath == nullptr) {
    return;
  }

  outPath->fill('\0');
  core::copy_string(outPath->data(), outPath->size(), sourcePath);
}

} // namespace

/// Drops all pending requests.
void clear_asset_request_queue(AssetRequestQueue *queue) noexcept {
  if (queue == nullptr) {
    return;
  }

  queue->requests.fill(AssetRequest{});
  queue->requestHead = 0U;
  queue->requestCount = 0U;
  queue->droppedRequests = 0U;
}

/// Number of queued transitions.
std::size_t pending_asset_request_count(
    const AssetRequestQueue *queue) noexcept {
  if (queue == nullptr) {
    return 0U;
  }

  return queue->requestCount;
}

/// Enqueues a transition; warns once per overflow episode.
bool push_asset_request(AssetRequestQueue *queue, AssetRequestType type,
                        AssetId id, const char *sourcePath) noexcept {
  if ((queue == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  if (queue->requestCount >= queue->requests.size()) {
    ++queue->droppedRequests;
    if (queue->droppedRequests == 1U) {
      core::log_message(core::LogLevel::Warning, "assets",
                        "asset request ring full; requests are being "
                        "dropped (droppedRequests counts the total)");
    }
    return false;
  }

  const std::size_t slot =
      (queue->requestHead + queue->requestCount) % queue->requests.size();
  AssetRequest &request = queue->requests[slot];
  request = AssetRequest{};
  request.type = type;
  request.id = id;
  copy_source_path(&request.sourcePath, sourcePath);
  ++queue->requestCount;
  return true;
}

/// Pops the oldest transition; false when the ring is empty.
bool pop_asset_request(AssetRequestQueue *queue,
                       AssetRequest *outRequest) noexcept {
  if ((queue == nullptr) || (outRequest == nullptr) ||
      (queue->requestCount == 0U)) {
    return false;
  }

  *outRequest = queue->requests[queue->requestHead];
  queue->requests[queue->requestHead] = AssetRequest{};
  queue->requestHead = (queue->requestHead + 1U) % queue->requests.size();
  --queue->requestCount;
  return true;
}

/// True when a transition of this type is already queued for the id.
bool has_pending_asset_request(const AssetRequestQueue *queue,
                               AssetRequestType type, AssetId id) noexcept {
  if ((queue == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  for (std::size_t i = 0U; i < queue->requestCount; ++i) {
    const std::size_t slot =
        (queue->requestHead + i) % queue->requests.size();
    const AssetRequest &request = queue->requests[slot];
    if ((request.id == id) && (request.type == type)) {
      return true;
    }
  }

  return false;
}

} // namespace engine::content
