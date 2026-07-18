// Declares asset manager types and APIs for the Engine renderer system.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/mesh_loader.h"

namespace engine::renderer {

/// Enumerates asset request type values used by the engine.
enum class AssetRequestType : std::uint8_t { Load, Unload, Reload };

/// One queued load/unload/reload transition for an asset id.
struct AssetRequest final {
  AssetRequestType type = AssetRequestType::Load;
  AssetId id = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
};

/// Fixed queue of asset transitions applied by update_asset_manager.
struct AssetManager final {
  static constexpr std::size_t kMaxQueuedRequests = 1024U;

  std::array<AssetRequest, kMaxQueuedRequests> requests{};
  std::size_t requestHead = 0U;
  std::size_t requestCount = 0U;
  std::uint32_t droppedRequests = 0U;
};

/// Drops all pending requests.
void clear_asset_manager(AssetManager *manager) noexcept;
/// Number of queued transitions.
std::size_t pending_asset_request_count(const AssetManager *manager) noexcept;

/// Queues a mesh load; false when the queue is full.
bool queue_mesh_load(AssetManager *manager,
                     AssetDatabase *database,
                     AssetId id,
                     const char *sourcePath) noexcept;
/// Queues a mesh unload; false when the queue is full.
bool queue_mesh_unload(AssetManager *manager,
                       AssetDatabase *database,
                       AssetId id) noexcept;
/// Queues a mesh reload (unload + load); false when the queue is full.
bool queue_mesh_reload(AssetManager *manager,
                       AssetDatabase *database,
                       AssetId id,
                       const char *sourcePath) noexcept;

// Processes up to maxTransitions queued transitions and auto-synchronizes
// requested residency intent from the asset database into explicit queues.
bool update_asset_manager(AssetManager *manager,
                          AssetDatabase *database,
                          GpuMeshRegistry *registry,
                          std::size_t maxTransitions) noexcept;

/// Shuts down the owning system for asset manager.
void shutdown_asset_manager(AssetManager *manager,
                            AssetDatabase *database,
                            GpuMeshRegistry *registry) noexcept;

} // namespace engine::renderer
