// Declares the runtime async mesh-streaming bridge: transfer-slot state
// shared between the streaming worker and the main thread, the queue
// callbacks, and failure synchronization.

#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "engine/renderer/asset_database.h"
#include "engine/content/asset_streaming.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/runtime/service_registry.h"

namespace engine {

namespace renderer {
struct GpuMeshRegistry;
}

/// CPU mesh payloads loaded by the streaming worker and consumed on the
/// render thread.
struct StreamingMeshTransferSlot final {
  renderer::AssetId assetId = renderer::kInvalidAssetId;
  renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  bool occupied = false;
};

/// Mutex-guarded transfer slots plus the destinations uploads resolve into.
struct RuntimeAssetStreamingState final {
  renderer::AssetDatabase *database = nullptr;
  renderer::GpuMeshRegistry *meshRegistry = nullptr;
  std::array<StreamingMeshTransferSlot,
             content::AssetStreamingQueue::kMaxRequests>
      meshTransfers{};
  std::mutex mutex{};
};

/// Streaming-queue load callback: worker-thread CPU IO into a transfer slot.
bool runtime_streaming_load_mesh(renderer::AssetId assetId, const char *path,
                                 std::uint64_t *outSizeBytes,
                                 void *userData) noexcept;

/// Streaming-queue upload callback: main-thread GPU upload from the slot.
bool runtime_streaming_upload_mesh(renderer::AssetId assetId,
                                   void *userData) noexcept;

/// Releases every transfer slot's CPU payload (teardown path).
void clear_streamed_mesh_data(RuntimeAssetStreamingState *state) noexcept;

/// Retires terminal script streaming handles: mirrors Failed into the
/// database, releases the queue slot, and clears the stale handle reference.
void retire_terminal_script_loads(
    runtime::EngineAssetDatabaseService *service) noexcept;

/// Mirrors terminal streaming results into the asset database and releases
/// the terminal queue slots so the fixed request table cannot exhaust.
void sync_streaming_failures(
    runtime::EngineAssetDatabaseService *service) noexcept;

} // namespace engine
