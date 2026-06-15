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

/// Stores asset request data used by the engine.
struct AssetRequest final {
  AssetRequestType type = AssetRequestType::Load;
  AssetId id = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
};

/// Stores asset manager data used by the engine.
struct AssetManager final {
  static constexpr std::size_t kMaxQueuedRequests = 1024U;

  std::array<AssetRequest, kMaxQueuedRequests> requests{};
  std::size_t requestHead = 0U;
  std::size_t requestCount = 0U;
  std::uint32_t droppedRequests = 0U;
};

/// Handles clear asset manager.
void clear_asset_manager(AssetManager *manager) noexcept;
/// Handles pending asset request count.
std::size_t pending_asset_request_count(const AssetManager *manager) noexcept;

/// Handles queue mesh load.
bool queue_mesh_load(AssetManager *manager,
                     AssetDatabase *database,
                     AssetId id,
                     const char *sourcePath) noexcept;
/// Handles queue mesh unload.
bool queue_mesh_unload(AssetManager *manager,
                       AssetDatabase *database,
                       AssetId id) noexcept;
/// Handles queue mesh reload.
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
