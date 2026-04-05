#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/mesh_loader.h"

namespace engine::renderer {

enum class AssetRequestType : std::uint8_t { Load, Unload, Reload };

struct AssetRequest final {
  AssetRequestType type = AssetRequestType::Load;
  AssetId id = kInvalidAssetId;
  std::array<char, 260U> sourcePath{};
};

struct AssetManager final {
  static constexpr std::size_t kMaxQueuedRequests = 1024U;

  std::array<AssetRequest, kMaxQueuedRequests> requests{};
  std::size_t requestHead = 0U;
  std::size_t requestCount = 0U;
  std::uint32_t droppedRequests = 0U;
};

void clear_asset_manager(AssetManager *manager) noexcept;
std::size_t pending_asset_request_count(const AssetManager *manager) noexcept;

bool queue_mesh_load(AssetManager *manager,
                     AssetDatabase *database,
                     AssetId id,
                     const char *sourcePath) noexcept;
bool queue_mesh_unload(AssetManager *manager,
                       AssetDatabase *database,
                       AssetId id) noexcept;
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

void shutdown_asset_manager(AssetManager *manager,
                            AssetDatabase *database,
                            GpuMeshRegistry *registry) noexcept;

} // namespace engine::renderer
