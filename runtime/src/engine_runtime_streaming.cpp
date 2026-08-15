// Implements the runtime side of async mesh streaming: worker-thread CPU
// loads staged into transfer slots, main-thread GPU uploads, and failure
// synchronization back into the asset database.

#include "engine_runtime_streaming.h"

#include <cstddef>
#include <mutex>
#include <utility>

#include "engine/core/logging.h"
#include "engine/renderer/mesh_loader.h"

namespace engine {

/// Stores a CPU-side mesh payload for main-thread upload.
bool store_streamed_mesh_data(RuntimeAssetStreamingState *state,
                              renderer::AssetId assetId,
                              renderer::CpuMeshData &&meshData,
                              std::uint64_t sizeBytes) noexcept {
  if ((state == nullptr) || (assetId == renderer::kInvalidAssetId)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  std::size_t freeSlot = state->meshTransfers.size();
  for (std::size_t i = 0U; i < state->meshTransfers.size(); ++i) {
    StreamingMeshTransferSlot &slot = state->meshTransfers[i];
    if (slot.occupied && (slot.assetId == assetId)) {
      slot.meshData = std::move(meshData);
      slot.sizeBytes = sizeBytes;
      return true;
    }
    if (!slot.occupied && (freeSlot == state->meshTransfers.size())) {
      freeSlot = i;
    }
  }

  if (freeSlot == state->meshTransfers.size()) {
    return false;
  }

  StreamingMeshTransferSlot &slot = state->meshTransfers[freeSlot];
  slot.assetId = assetId;
  slot.meshData = std::move(meshData);
  slot.sizeBytes = sizeBytes;
  slot.occupied = true;
  return true;
}

/// Takes a CPU-side mesh payload loaded by the streaming worker.
bool take_streamed_mesh_data(RuntimeAssetStreamingState *state,
                             renderer::AssetId assetId,
                             renderer::CpuMeshData *outMeshData,
                             std::uint64_t *outSizeBytes) noexcept {
  if ((state == nullptr) || (assetId == renderer::kInvalidAssetId) ||
      (outMeshData == nullptr)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  for (StreamingMeshTransferSlot &slot : state->meshTransfers) {
    if (!slot.occupied || (slot.assetId != assetId)) {
      continue;
    }

    *outMeshData = std::move(slot.meshData);
    if (outSizeBytes != nullptr) {
      *outSizeBytes = slot.sizeBytes;
    }
    slot = StreamingMeshTransferSlot{};
    return true;
  }
  return false;
}

/// Clears any CPU-side mesh payloads not consumed by upload.
void clear_streamed_mesh_data(RuntimeAssetStreamingState *state) noexcept {
  if (state == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(state->mutex);
  for (StreamingMeshTransferSlot &slot : state->meshTransfers) {
    slot = StreamingMeshTransferSlot{};
  }
}

/// Worker-thread CPU load callback for runtime asset streaming.
bool runtime_streaming_load_mesh(renderer::AssetId assetId, const char *path,
                                 std::uint64_t *outSizeBytes,
                                 void *userData) noexcept {
  auto *state = static_cast<RuntimeAssetStreamingState *>(userData);
  renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  if (!renderer::load_mesh_data_from_file(path, &meshData, &sizeBytes)) {
    return false;
  }

  if (!store_streamed_mesh_data(state, assetId, std::move(meshData),
                                sizeBytes)) {
    return false;
  }

  if (outSizeBytes != nullptr) {
    *outSizeBytes = sizeBytes;
  }
  return true;
}

/// Main-thread GPU upload callback for runtime asset streaming.
bool runtime_streaming_upload_mesh(renderer::AssetId assetId,
                                   void *userData) noexcept {
  auto *state = static_cast<RuntimeAssetStreamingState *>(userData);
  if ((state == nullptr) || (state->database == nullptr) ||
      (state->meshRegistry == nullptr)) {
    return false;
  }

  renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  if (!take_streamed_mesh_data(state, assetId, &meshData, &sizeBytes)) {
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Failed,
        renderer::kInvalidMeshHandle));
    return false;
  }

  if (!renderer::mesh_asset_requested_resident(state->database, assetId) ||
      (renderer::mesh_asset_state(state->database, assetId) !=
       renderer::AssetState::Loading)) {
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Unloaded,
        renderer::kInvalidMeshHandle));
    return true;
  }

  renderer::GpuMesh mesh{};
  if (!renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Failed,
        renderer::kInvalidMeshHandle));
    return false;
  }

  const renderer::MeshHandle meshHandle =
      renderer::register_gpu_mesh(state->meshRegistry, mesh);
  if (meshHandle == renderer::kInvalidMeshHandle) {
    renderer::unload_mesh(&mesh);
    static_cast<void>(renderer::set_mesh_asset_state(
        state->database, assetId, renderer::AssetState::Failed,
        renderer::kInvalidMeshHandle));
    core::log_message(core::LogLevel::Error, "assets",
                      "mesh registry is full; streamed asset upload failed");
    return false;
  }

  if (!renderer::set_mesh_asset_state(state->database, assetId,
                                      renderer::AssetState::Ready,
                                      meshHandle)) {
    // Roll back through the public unload path (not raw slot indexing) so
    // the registry's generation bump still fires (audit #173).
    renderer::unload_gpu_mesh(state->meshRegistry, meshHandle);
    return false;
  }

  static_cast<void>(
      renderer::set_mesh_asset_size(state->database, assetId, sizeBytes));
  return true;
}

/// Retires script handles whose streaming request reached a terminal state.
void retire_terminal_script_loads(
    runtime::EngineAssetDatabaseService *service) noexcept {
  if ((service == nullptr) || (service->database == nullptr) ||
      (service->streamingQueue == nullptr)) {
    return;
  }

  for (auto &handle : service->scriptLoadHandles) {
    if (!handle.occupied || !handle.streamingHandle.valid() ||
        (handle.assetId == renderer::kInvalidAssetId)) {
      continue;
    }

    const renderer::LoadingState state = renderer::get_load_state(
        service->streamingQueue, handle.streamingHandle);
    if (state == renderer::LoadingState::Failed) {
      static_cast<void>(renderer::set_mesh_asset_state(
          service->database, handle.assetId, renderer::AssetState::Failed,
          renderer::kInvalidMeshHandle));
      static_cast<void>(renderer::release_load(service->streamingQueue,
                                               handle.streamingHandle));
      handle.streamingHandle = renderer::kInvalidLoadHandle;
    } else if ((state == renderer::LoadingState::Ready) &&
               (renderer::mesh_asset_state(service->database,
                                           handle.assetId) ==
                renderer::AssetState::Ready)) {
      static_cast<void>(renderer::release_load(service->streamingQueue,
                                               handle.streamingHandle));
      handle.streamingHandle = renderer::kInvalidLoadHandle;
    }
  }
}

/// Mirrors terminal streaming results into the asset database on the main
/// thread and releases the terminal queue slots for reuse.
void sync_streaming_failures(
    runtime::EngineAssetDatabaseService *service) noexcept {
  if ((service == nullptr) || (service->database == nullptr) ||
      (service->streamingQueue == nullptr)) {
    return;
  }

  retire_terminal_script_loads(service);

  renderer::AssetStreamingQueue *queue = service->streamingQueue;
  struct TerminalRequest final {
    renderer::LoadHandle handle{};
    renderer::AssetId assetId = renderer::kInvalidAssetId;
    renderer::LoadingState state = renderer::LoadingState::Queued;
  };
  std::array<TerminalRequest, renderer::AssetStreamingQueue::kMaxRequests>
      terminals{};
  std::size_t terminalCount = 0U;

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    for (std::uint32_t i = 0U;
         i < renderer::AssetStreamingQueue::kMaxRequests; ++i) {
      const renderer::LoadRequest &request = queue->requests[i];
      if (!request.occupied ||
          ((request.state != renderer::LoadingState::Ready) &&
           (request.state != renderer::LoadingState::Failed))) {
        continue;
      }
      terminals[terminalCount].handle =
          renderer::LoadHandle{i, request.generation};
      terminals[terminalCount].assetId = request.assetId;
      terminals[terminalCount].state = request.state;
      ++terminalCount;
    }
  }

  for (std::size_t i = 0U; i < terminalCount; ++i) {
    if ((terminals[i].state == renderer::LoadingState::Failed) &&
        (renderer::mesh_asset_state(service->database, terminals[i].assetId) ==
         renderer::AssetState::Loading)) {
      static_cast<void>(renderer::set_mesh_asset_state(
          service->database, terminals[i].assetId, renderer::AssetState::Failed,
          renderer::kInvalidMeshHandle));
    }
    static_cast<void>(
        renderer::release_load(queue, terminals[i].handle));
  }
}


} // namespace engine
