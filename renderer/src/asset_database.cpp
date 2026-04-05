#include "engine/renderer/asset_database.h"

#include <cstddef>
#include <cstring>

namespace engine::renderer {

namespace {

constexpr std::uint32_t kFnvOffset = 2166136261U;
constexpr std::uint32_t kFnvPrime = 16777619U;

std::size_t hashed_slot(AssetId id, std::size_t capacity) noexcept {
  if ((capacity == 0U) || (id == kInvalidAssetId)) {
    return 0U;
  }

  return static_cast<std::size_t>(id) % capacity;
}

std::size_t find_mesh_asset_slot(const AssetDatabase *database,
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

std::size_t find_mesh_asset_insert_slot(const AssetDatabase *database,
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

void write_source_path(std::array<char, 260U> *outPath,
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

} // namespace

AssetId make_asset_id_from_path(const char *path) noexcept {
  if (path == nullptr) {
    return kInvalidAssetId;
  }

  std::uint32_t hash = kFnvOffset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(path);
       *cursor != 0U;
       ++cursor) {
    const unsigned char ch = (*cursor == static_cast<unsigned char>('\\'))
                                 ? static_cast<unsigned char>('/')
                                 : *cursor;
    hash ^= static_cast<std::uint32_t>(ch);
    hash *= kFnvPrime;
  }

  if (hash == kInvalidAssetId) {
    hash = 1U;
  }

  return hash;
}

bool register_mesh_asset(AssetDatabase *database,
                         AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)
      || (runtimeMesh == kInvalidMeshHandle)) {
    return false;
  }

  const std::size_t slot = find_mesh_asset_insert_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  database->occupied[slot] = true;
  MeshAssetRecord &record = database->meshAssets[slot];
  record.id = id;
  record.runtimeMesh = runtimeMesh;
  record.refCount = (record.refCount == 0U) ? 1U : record.refCount;
  record.state = AssetState::Ready;
  record.requestedResident = true;
  write_source_path(&record.sourcePath, sourcePath);
  return true;
}

AssetState mesh_asset_state(const AssetDatabase *database,
                            AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return AssetState::Unloaded;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return AssetState::Unloaded;
  }

  return database->meshAssets[slot].state;
}

bool set_mesh_asset_state(AssetDatabase *database,
                          AssetId id,
                          AssetState state,
                          MeshHandle runtimeMesh) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  if ((state == AssetState::Ready) && (runtimeMesh == kInvalidMeshHandle)) {
    return false;
  }

  record.state = state;
  if (state == AssetState::Ready) {
    record.runtimeMesh = runtimeMesh;
  } else {
    record.runtimeMesh = kInvalidMeshHandle;
  }

  return true;
}

bool mesh_asset_requested_resident(const AssetDatabase *database,
                                   AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  return database->meshAssets[slot].requestedResident;
}

MeshHandle resolve_mesh_asset(const AssetDatabase *database,
                              AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return kInvalidMeshHandle;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return kInvalidMeshHandle;
  }

  const MeshAssetRecord &record = database->meshAssets[slot];
  if (record.state != AssetState::Ready) {
    return kInvalidMeshHandle;
  }

  return record.runtimeMesh;
}

bool retain_mesh_asset(AssetDatabase *database, AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  ++record.refCount;
  record.requestedResident = true;
  return true;
}

bool release_mesh_asset(AssetDatabase *database, AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  if (record.refCount > 0U) {
    --record.refCount;
  }

  if (record.refCount == 0U) {
    record.requestedResident = false;
  }

  return true;
}

void clear_asset_database(AssetDatabase *database) noexcept {
  if (database == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
    database->occupied[i] = false;
    database->meshAssets[i] = MeshAssetRecord{};
  }
}

} // namespace engine::renderer
