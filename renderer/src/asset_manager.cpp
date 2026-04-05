#include "engine/renderer/asset_manager.h"

#include <cstddef>
#include <cstring>

#include "engine/core/logging.h"

namespace engine::renderer {

namespace {

std::size_t hashed_slot(AssetId id, std::size_t capacity) noexcept {
  if ((capacity == 0U) || (id == kInvalidAssetId)) {
    return 0U;
  }

  return static_cast<std::size_t>(id) % capacity;
}

std::size_t find_record_slot(const AssetDatabase *database,
                             AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->meshAssets.size() : 0U;
  }

  const std::size_t capacity = database->meshAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->occupied[slot]) {
      return database->meshAssets.size();
    }

    if (database->meshAssets[slot].id == id) {
      return slot;
    }
  }

  return database->meshAssets.size();
}

std::size_t find_record_insert_slot(const AssetDatabase *database,
                                    AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }

  const std::size_t capacity = database->meshAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->occupied[slot] || (database->meshAssets[slot].id == id)) {
      return slot;
    }
  }

  return database->meshAssets.size();
}

void copy_source_path(std::array<char, 260U> *outPath,
                      const char *sourcePath) noexcept {
  if (outPath == nullptr) {
    return;
  }

  outPath->fill('\0');
  if (sourcePath == nullptr) {
    return;
  }

  const std::size_t maxCopy = outPath->size() - 1U;
  const std::size_t sourceLength = std::strlen(sourcePath);
  const std::size_t copyLength =
      (sourceLength > maxCopy) ? maxCopy : sourceLength;
  if (copyLength > 0U) {
    std::memcpy(outPath->data(), sourcePath, copyLength);
  }
  (*outPath)[copyLength] = '\0';
}

bool has_source_path(const MeshAssetRecord &record) noexcept {
  return record.sourcePath[0] != '\0';
}

bool has_pending_request(const AssetManager *manager,
                         AssetRequestType type,
                         AssetId id) noexcept {
  if ((manager == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  for (std::size_t i = 0U; i < manager->requestCount; ++i) {
    const std::size_t slot =
        (manager->requestHead + i) % manager->requests.size();
    const AssetRequest &request = manager->requests[slot];
    if ((request.id == id) && (request.type == type)) {
      return true;
    }
  }

  return false;
}

bool push_request(AssetManager *manager,
                  AssetRequestType type,
                  AssetId id,
                  const char *sourcePath) noexcept {
  if ((manager == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  if (manager->requestCount >= manager->requests.size()) {
    ++manager->droppedRequests;
    return false;
  }

  const std::size_t slot =
      (manager->requestHead + manager->requestCount) % manager->requests.size();
  AssetRequest &request = manager->requests[slot];
  request = AssetRequest{};
  request.type = type;
  request.id = id;
  copy_source_path(&request.sourcePath, sourcePath);
  ++manager->requestCount;
  return true;
}

bool pop_request(AssetManager *manager, AssetRequest *outRequest) noexcept {
  if ((manager == nullptr) || (outRequest == nullptr)
      || (manager->requestCount == 0U)) {
    return false;
  }

  *outRequest = manager->requests[manager->requestHead];
  manager->requests[manager->requestHead] = AssetRequest{};
  manager->requestHead = (manager->requestHead + 1U) % manager->requests.size();
  --manager->requestCount;
  return true;
}

bool ensure_record(AssetDatabase *database,
                   AssetId id,
                   const char *sourcePath,
                   std::size_t *outSlot) noexcept {
  if ((database == nullptr) || (outSlot == nullptr)
      || (id == kInvalidAssetId)) {
    return false;
  }

  std::size_t slot = find_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    slot = find_record_insert_slot(database, id);
    if (slot == database->meshAssets.size()) {
      return false;
    }

    database->occupied[slot] = true;
    database->meshAssets[slot] = MeshAssetRecord{};
    database->meshAssets[slot].id = id;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  if ((sourcePath != nullptr) && (sourcePath[0] != '\0')) {
    copy_source_path(&record.sourcePath, sourcePath);
  }

  *outSlot = slot;
  return true;
}

void unload_registry_mesh(GpuMeshRegistry *registry,
                          MeshHandle handle) noexcept {
  if ((registry == nullptr) || (handle == kInvalidMeshHandle)) {
    return;
  }

  const std::uint32_t meshId = handle.id;
  if ((meshId == 0U) || (meshId >= registry->meshes.size())) {
    return;
  }

  if (!registry->occupied[meshId]) {
    return;
  }

  unload_mesh(&registry->meshes[meshId]);
  registry->occupied[meshId] = false;
  registry->meshes[meshId] = GpuMesh{};
}

void unload_record_mesh(MeshAssetRecord *record,
                        GpuMeshRegistry *registry) noexcept {
  if (record == nullptr) {
    return;
  }

  unload_registry_mesh(registry, record->runtimeMesh);
  record->runtimeMesh = kInvalidMeshHandle;
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
        if (!has_pending_request(
                manager, AssetRequestType::Unload, record.id)) {
          static_cast<void>(push_request(
              manager, AssetRequestType::Unload, record.id, nullptr));
        }
      }
      continue;
    }

    if (record.state == AssetState::Unloaded) {
      if (!has_pending_request(manager, AssetRequestType::Load, record.id)
          && !has_pending_request(
              manager, AssetRequestType::Reload, record.id)) {
        const char *path =
            has_source_path(record) ? record.sourcePath.data() : nullptr;
        static_cast<void>(
            push_request(manager, AssetRequestType::Load, record.id, path));
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

  const std::uint32_t meshSlot = register_gpu_mesh(registry, mesh);
  if (meshSlot == 0U) {
    unload_mesh(&mesh);
    record.state =
        record.requestedResident ? AssetState::Failed : AssetState::Unloaded;
    record.runtimeMesh = kInvalidMeshHandle;
    core::log_message(core::LogLevel::Error,
                      "assets",
                      "mesh registry is full; cannot promote asset to Ready");
    return false;
  }

  record.runtimeMesh = MeshHandle{meshSlot};
  if (!record.requestedResident || (record.refCount == 0U)) {
    unload_record_mesh(&record, registry);
    record.state = AssetState::Unloaded;
    return true;
  }

  record.state = AssetState::Ready;
  return true;
}

} // namespace

void clear_asset_manager(AssetManager *manager) noexcept {
  if (manager == nullptr) {
    return;
  }

  manager->requests.fill(AssetRequest{});
  manager->requestHead = 0U;
  manager->requestCount = 0U;
  manager->droppedRequests = 0U;
}

std::size_t pending_asset_request_count(const AssetManager *manager) noexcept {
  if (manager == nullptr) {
    return 0U;
  }

  return manager->requestCount;
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

  if (has_pending_request(manager, AssetRequestType::Load, id)
      || has_pending_request(manager, AssetRequestType::Reload, id)) {
    return true;
  }

  return push_request(manager, AssetRequestType::Load, id, sourcePath);
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

  if (has_pending_request(manager, AssetRequestType::Unload, id)) {
    return true;
  }

  return push_request(manager, AssetRequestType::Unload, id, nullptr);
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

  if (has_pending_request(manager, AssetRequestType::Reload, id)) {
    return true;
  }

  return push_request(manager, AssetRequestType::Reload, id, sourcePath);
}

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
  while ((processed < maxTransitions) && pop_request(manager, &request)) {
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
