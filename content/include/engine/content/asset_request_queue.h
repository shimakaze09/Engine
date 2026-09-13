// Declares the generic asset transition-request queue (#171 C3): the fixed
// ring of load/unload/reload requests split out of the renderer's asset
// manager, usable by any per-type residency service with no renderer
// dependency.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/content/asset_metadata.h"
#include "engine/core/fixed_ring.h"

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
/// A full ring refuses rather than overwrites: dropping a queued
/// transition silently would lose a residency change, so the refusal is
/// counted in droppedRequests for the owner to report.
struct AssetRequestQueue final {
  static constexpr std::size_t kMaxQueuedRequests = 1024U;

  core::FixedRing<AssetRequest, kMaxQueuedRequests> requests{};
  std::uint32_t droppedRequests = 0U;
};

/// Drops all pending requests.
void clear_asset_request_queue(AssetRequestQueue *queue) noexcept;

/// Number of queued transitions.
std::size_t pending_asset_request_count(const AssetRequestQueue *queue) noexcept;

/// Queued transition `index` counted from the oldest (0); nullptr when
/// out of range. Read-only inspection for diagnostics and tests; draining
/// goes through pop_asset_request.
const AssetRequest *pending_asset_request_at(const AssetRequestQueue *queue,
                                             std::size_t index) noexcept;

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
