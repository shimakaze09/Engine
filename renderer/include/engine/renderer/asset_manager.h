// Declares the renderer's mesh residency service: queues mesh transition
// requests into the content-owned AssetRequestQueue (#171 C3) and applies
// them against the asset database's mesh records and the GPU mesh registry.

#pragma once

#include <cstddef>

#include "engine/content/asset_request_queue.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/mesh_loader.h"

namespace engine::renderer {

// The queue container and request vocabulary are content-owned (#171 C3);
// these names keep the renderer's established service vocabulary working.
using content::AssetRequest;
using content::AssetRequestType;
using AssetManager = content::AssetRequestQueue;
using content::pending_asset_request_count;

/// Drops all pending requests.
void clear_asset_manager(AssetManager *manager) noexcept;

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
