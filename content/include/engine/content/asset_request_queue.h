// Declares the generic asset transition-request queue (#171 C3): the fixed
// ring of load/unload/reload requests split out of the renderer's asset
// manager, usable by any per-type residency service with no renderer
// dependency.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/content/asset_metadata.h"

namespace engine::content {

/// Enumerates asset request type values used by the engine.
enum class AssetRequestType : std::uint8_t { Load, Unload, Reload };

/// One queued load/unload/reload transition for an asset id.
struct AssetRequest final {
  AssetRequestType type = AssetRequestType::Load;
  AssetId id = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
};

/// Fixed transition-request ring drained by a per-type residency service.
struct AssetRequestQueue final {
  static constexpr std::size_t kMaxQueuedRequests = 1024U;

  std::array<AssetRequest, kMaxQueuedRequests> requests{};
  std::size_t requestHead = 0U;
  std::size_t requestCount = 0U;
  std::uint32_t droppedRequests = 0U;
};

/// Drops all pending requests.
void clear_asset_request_queue(AssetRequestQueue *queue) noexcept;

/// Number of queued transitions.
std::size_t pending_asset_request_count(const AssetRequestQueue *queue) noexcept;

/// Enqueues a transition; false (and a once-per-overflow-episode warning,
/// droppedRequests counting the total) when the ring is full.
bool push_asset_request(AssetRequestQueue *queue, AssetRequestType type,
                        AssetId id, const char *sourcePath) noexcept;

/// Pops the oldest transition; false when the ring is empty.
bool pop_asset_request(AssetRequestQueue *queue,
                       AssetRequest *outRequest) noexcept;

/// True when a transition of this type is already queued for the id.
bool has_pending_asset_request(const AssetRequestQueue *queue,
                               AssetRequestType type, AssetId id) noexcept;

} // namespace engine::content
