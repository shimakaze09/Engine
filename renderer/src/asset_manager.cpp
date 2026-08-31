// Implements asset manager behavior for the Engine renderer system.

#include "engine/renderer/asset_manager.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "engine/core/logging.h"
#include "engine/core/string_util.h"

namespace engine::renderer {

namespace {

/// Resolves an id to its record slot via the database's shared probe logic.
std::size_t find_record_slot(const AssetDatabase *database,
                             AssetId id) noexcept {
  return find_mesh_asset_record_slot(database, id);
}

/// Copies a mesh source path into a record field (zero-fills the tail so
/// records stay byte-comparable).
void copy_source_path(std::array<char, 260U> *outPath,
                      const char *sourcePath) noexcept {
  if (outPath == nullptr) {
    return;
  }

  outPath->fill('\0');
  core::copy_string(outPath->data(), outPath->size(), sourcePath);
}

/// Returns whether has source path.
bool has_source_path(const MeshAssetRecord &record) noexcept {
  return record.sourcePath[0] != '\0';
}

bool ensure_record(AssetDatabase *database,
                   AssetId id,
                   const char *sourcePath,
                   std::size_t *outSlot) noexcept {
  if ((database == nullptr) || (outSlot == nullptr)
      || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = claim_mesh_asset_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  if ((sourcePath != nullptr) && (sourcePath[0] != '\0')) {
    copy_source_path(&record.sourcePath, sourcePath);
  }

  *outSlot = slot;
  return true;
}

void unload_record_mesh(MeshAssetRecord *record,
                        GpuMeshRegistry *registry) noexcept {
  if (record == nullptr) {
    return;
  }

  unload_gpu_mesh(registry, record->runtimeMesh);
  record->runtimeMesh = kInvalidMeshHandle;
}

/// Estimated payload size (positions/normals + optional UVs, 32-bit
/// indices) for cache-budget accounting; streamed loads store exact sizes.
std::uint64_t estimated_mesh_size_bytes(const GpuMesh &mesh) noexcept {
  const std::uint64_t vertexFloats = mesh.hasUVs ? 8ULL : 6ULL;
  return (static_cast<std::uint64_t>(mesh.vertexCount) * vertexFloats *
          sizeof(float)) +
         (static_cast<std::uint64_t>(mesh.indexCount) * sizeof(std::uint32_t));
}

void sync_requested_residency(AssetManager *manager,
                              AssetDatabase *database) noexcept {
  if ((manager == nullptr) || (database == nullptr)) {
    return;
  }

  for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
    if (!database->occupied[i]) {
      continue;
    }

    const MeshAssetRecord &record = database->meshAssets[i];
    if (record.id == kInvalidAssetId) {
      continue;
    }

    if (!record.requestedResident) {
      if ((record.state == AssetState::Ready)
          || (record.state == AssetState::Loading)
          || (record.state == AssetState::Failed)) {
        if (!content::has_pending_asset_request(
                manager, AssetRequestType::Unload, record.id)) {
          static_cast<void>(content::push_asset_request(
              manager, AssetRequestType::Unload, record.id, nullptr));
        }
      }
      continue;
    }

    if (record.state == AssetState::Unloaded) {
      if (!content::has_pending_asset_request(manager, AssetRequestType::Load, record.id)
          && !content::has_pending_asset_request(
              manager, AssetRequestType::Reload, record.id)) {
        const char *path =
            has_source_path(record) ? record.sourcePath.data() : nullptr;
        static_cast<void>(
            content::push_asset_request(manager, AssetRequestType::Load, record.id, path));
      }
    }
  }
}

bool process_load_like_request(AssetDatabase *database,
                               GpuMeshRegistry *registry,
                               const AssetRequest &request,
                               bool forceReload) noexcept {
  if ((database == nullptr) || (registry == nullptr)
      || (request.id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_record_slot(database, request.id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];

  // A forced reload over a live Ready mesh is staged (the authored-data
  // rule's failed-reload clause): the replacement is decoded and
  // registered under temporary ownership first, and only a fully valid
  // replacement retires the old mesh — exactly once, in the swap. Any
  // failure (missing/malformed input, registry capacity) returns with the
  // record byte-for-byte as it was: handle, state, source path, and
  // residency accounting all still describe the last valid resource.
  const bool stagedReload =
      forceReload && (record.state == AssetState::Ready) &&
      (record.runtimeMesh != kInvalidMeshHandle) && record.requestedResident &&
      (record.refCount > 0U);
  if (stagedReload) {
    const char *replacementPath = (request.sourcePath[0] != '\0')
                                      ? request.sourcePath.data()
                                      : (has_source_path(record)
                                             ? record.sourcePath.data()
                                             : nullptr);
    if (replacementPath == nullptr) {
      core::log_message(core::LogLevel::Error, "assets",
                        "mesh reload requested without source path; keeping "
                        "the previous mesh");
      return false;
    }

    GpuMesh replacementMesh{};
    if (!load_mesh_from_file(replacementPath, &replacementMesh)) {
      char message[640] = {};
      std::snprintf(message, sizeof(message),
                    "%s: mesh reload failed; keeping the previous mesh",
                    replacementPath);
      core::log_message(core::LogLevel::Error, "assets", message);
      return false;
    }

    const MeshHandle replacementHandle =
        register_gpu_mesh(registry, replacementMesh);
    if (replacementHandle == kInvalidMeshHandle) {
      unload_mesh(&replacementMesh);
      char message[640] = {};
      std::snprintf(message, sizeof(message),
                    "%s: mesh registry is full; keeping the previous mesh",
                    replacementPath);
      core::log_message(core::LogLevel::Error, "assets", message);
      return false;
    }

    unload_record_mesh(&record, registry);
    copy_source_path(&record.sourcePath, replacementPath);
    record.runtimeMesh = replacementHandle;
    record.state = AssetState::Ready;
    record.lastAccessFrame.store(database->currentFrame,
                                 std::memory_order_relaxed);
    record.sizeBytes = estimated_mesh_size_bytes(replacementMesh);
    return true;
  }

  if ((request.sourcePath[0] != '\0')) {
    copy_source_path(&record.sourcePath, request.sourcePath.data());
  }

  if (forceReload) {
    unload_record_mesh(&record, registry);
    record.state = AssetState::Unloaded;
  }

  if (!record.requestedResident || (record.refCount == 0U)) {
    unload_record_mesh(&record, registry);
    record.state = AssetState::Unloaded;
    return true;
  }

  if (!has_source_path(record)) {
    record.state = AssetState::Failed;
    record.runtimeMesh = kInvalidMeshHandle;
    core::log_message(core::LogLevel::Error,
                      "assets",
                      "mesh load requested without source path");
    return false;
  }

  record.state = AssetState::Loading;

  GpuMesh mesh{};
  if (!load_mesh_from_file(record.sourcePath.data(), &mesh)) {
    record.state =
        record.requestedResident ? AssetState::Failed : AssetState::Unloaded;
    record.runtimeMesh = kInvalidMeshHandle;
    return false;
  }

  const MeshHandle meshHandle = register_gpu_mesh(registry, mesh);
  if (meshHandle == kInvalidMeshHandle) {
    unload_mesh(&mesh);
    record.state =
        record.requestedResident ? AssetState::Failed : AssetState::Unloaded;
    record.runtimeMesh = kInvalidMeshHandle;
    char message[640] = {};
    std::snprintf(message, sizeof(message),
                  "%s: mesh registry is full; cannot promote asset to Ready",
                  has_source_path(record) ? record.sourcePath.data()
                                          : "(no source path)");
    core::log_message(core::LogLevel::Error, "assets", message);
    return false;
  }

  record.runtimeMesh = meshHandle;
  if (!record.requestedResident || (record.refCount == 0U)) {
    unload_record_mesh(&record, registry);
    record.state = AssetState::Unloaded;
    return true;
  }

  record.state = AssetState::Ready;
  record.lastAccessFrame.store(database->currentFrame,
                               std::memory_order_relaxed);
  record.sizeBytes = estimated_mesh_size_bytes(mesh);
  return true;
}

} // namespace

void clear_asset_manager(AssetManager *manager) noexcept {
  content::clear_asset_request_queue(manager);
}

bool queue_mesh_load(AssetManager *manager,
                     AssetDatabase *database,
                     AssetId id,
                     const char *sourcePath) noexcept {
  if ((manager == nullptr) || (database == nullptr)
      || (id == kInvalidAssetId)) {
    return false;
  }

  std::size_t slot = 0U;
  if (!ensure_record(database, id, sourcePath, &slot)) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  record.requestedResident = true;
  if (record.refCount == 0U) {
    record.refCount = 1U;
  }

  if (record.state == AssetState::Ready) {
    return true;
  }

  if (content::has_pending_asset_request(manager, AssetRequestType::Load, id)
      || content::has_pending_asset_request(manager, AssetRequestType::Reload, id)) {
    return true;
  }

  return content::push_asset_request(manager, AssetRequestType::Load, id, sourcePath);
}

bool queue_mesh_unload(AssetManager *manager,
                       AssetDatabase *database,
                       AssetId id) noexcept {
  if ((manager == nullptr) || (database == nullptr)
      || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  record.requestedResident = false;
  record.refCount = 0U;

  if (record.state == AssetState::Unloaded) {
    return true;
  }

  if (content::has_pending_asset_request(manager, AssetRequestType::Unload, id)) {
    return true;
  }

  return content::push_asset_request(manager, AssetRequestType::Unload, id, nullptr);
}

bool queue_mesh_reload(AssetManager *manager,
                       AssetDatabase *database,
                       AssetId id,
                       const char *sourcePath) noexcept {
  if ((manager == nullptr) || (database == nullptr)
      || (id == kInvalidAssetId)) {
    return false;
  }

  std::size_t slot = 0U;
  if (!ensure_record(database, id, sourcePath, &slot)) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  record.requestedResident = true;
  if (record.refCount == 0U) {
    record.refCount = 1U;
  }

  if (content::has_pending_asset_request(manager, AssetRequestType::Reload, id)) {
    return true;
  }

  return content::push_asset_request(manager, AssetRequestType::Reload, id, sourcePath);
}

/// Advances this system for the current frame or tick for asset manager.
bool update_asset_manager(AssetManager *manager,
                          AssetDatabase *database,
                          GpuMeshRegistry *registry,
                          std::size_t maxTransitions) noexcept {
  if ((manager == nullptr) || (database == nullptr) || (registry == nullptr)) {
    return false;
  }

  if (maxTransitions == 0U) {
    return true;
  }

  sync_requested_residency(manager, database);

  bool allSucceeded = true;
  AssetRequest request{};
  std::size_t processed = 0U;
  while ((processed < maxTransitions) && content::pop_asset_request(manager, &request)) {
    const std::size_t slot = find_record_slot(database, request.id);
    if (slot == database->meshAssets.size()) {
      ++processed;
      continue;
    }

    MeshAssetRecord &record = database->meshAssets[slot];
    bool succeeded = true;

    switch (request.type) {
    case AssetRequestType::Load:
      succeeded = process_load_like_request(database, registry, request, false);
      break;
    case AssetRequestType::Reload:
      succeeded = process_load_like_request(database, registry, request, true);
      break;
    case AssetRequestType::Unload:
      unload_record_mesh(&record, registry);
      record.state = AssetState::Unloaded;
      record.runtimeMesh = kInvalidMeshHandle;
      break;
    }

    allSucceeded = allSucceeded && succeeded;
    ++processed;
  }

  return allSucceeded;
}

/// Shuts down the owning system for asset manager.
void shutdown_asset_manager(AssetManager *manager,
                            AssetDatabase *database,
                            GpuMeshRegistry *registry) noexcept {
  if ((database == nullptr) || (registry == nullptr)) {
    return;
  }

  if (manager != nullptr) {
    clear_asset_manager(manager);
  }

  for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
    if (!database->occupied[i]) {
      continue;
    }

    MeshAssetRecord &record = database->meshAssets[i];
    unload_record_mesh(&record, registry);
    record.state = AssetState::Unloaded;
    record.refCount = 0U;
    record.requestedResident = false;
  }
}

} // namespace engine::renderer
