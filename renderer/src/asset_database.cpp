#include "engine/renderer/asset_database.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

namespace {

constexpr std::uint64_t kFnv64Offset = 14695981039346656037ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

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

  std::uint64_t hash = kFnv64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(path);
       *cursor != 0U; ++cursor) {
    const unsigned char ch = (*cursor == static_cast<unsigned char>('\\'))
                                 ? static_cast<unsigned char>('/')
                                 : *cursor;
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnv64Prime;
  }

  if (hash == kInvalidAssetId) {
    hash = 1ULL;
  }

  return hash;
}

AssetId make_asset_id_from_file(const char *path) noexcept {
  if (path == nullptr) {
    return kInvalidAssetId;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return make_asset_id_from_path(path);
  }

  std::uint64_t hash = kFnv64Offset;
  unsigned char buffer[4096] = {};
  while (true) {
    const std::size_t bytesRead = std::fread(buffer, 1U, sizeof(buffer), file);
    if (bytesRead == 0U) {
      break;
    }
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash ^= static_cast<std::uint64_t>(buffer[i]);
      hash *= kFnv64Prime;
    }
  }

  std::fclose(file);
  if (hash == kInvalidAssetId) {
    hash = 1ULL;
  }
  return hash;
}

bool register_mesh_asset(AssetDatabase *database, AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) ||
      (runtimeMesh == kInvalidMeshHandle)) {
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

bool set_mesh_asset_state(AssetDatabase *database, AssetId id, AssetState state,
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

  for (std::size_t i = 0U; i < database->textureAssets.size(); ++i) {
    database->textureOccupied[i] = false;
    database->textureAssets[i] = TextureAssetRecord{};
  }

  for (std::size_t i = 0U; i < database->metadata.size(); ++i) {
    database->metadataOccupied[i] = false;
    database->metadata[i] = AssetMetadata{};
  }
}

// --- Texture asset functions ---

namespace {

std::size_t find_texture_slot(const AssetDatabase *database,
                              AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->textureAssets.size() : 0U;
  }

  const std::size_t capacity = database->textureAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->textureOccupied[slot]) {
      return capacity;
    }
    if (database->textureAssets[slot].id == id) {
      return slot;
    }
  }

  return capacity;
}

std::size_t find_texture_insert_slot(const AssetDatabase *database,
                                     AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }

  const std::size_t capacity = database->textureAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->textureOccupied[slot] ||
        (database->textureAssets[slot].id == id)) {
      return slot;
    }
  }

  return capacity;
}

} // namespace

bool register_texture_asset(AssetDatabase *database, AssetId id,
                            const char *sourcePath,
                            TextureHandle runtimeTexture) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) ||
      (runtimeTexture == kInvalidTextureHandle)) {
    return false;
  }

  const std::size_t slot = find_texture_insert_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return false;
  }

  database->textureOccupied[slot] = true;
  TextureAssetRecord &record = database->textureAssets[slot];
  record.id = id;
  record.runtimeTexture = runtimeTexture;
  record.refCount = (record.refCount == 0U) ? 1U : record.refCount;
  record.state = AssetState::Ready;
  record.requestedResident = true;
  write_source_path(&record.sourcePath, sourcePath);
  return true;
}

AssetState texture_asset_state(const AssetDatabase *database,
                               AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return AssetState::Unloaded;
  }

  const std::size_t slot = find_texture_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return AssetState::Unloaded;
  }

  return database->textureAssets[slot].state;
}

bool set_texture_asset_state(AssetDatabase *database, AssetId id,
                             AssetState state,
                             TextureHandle runtimeTexture) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_texture_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return false;
  }

  TextureAssetRecord &record = database->textureAssets[slot];
  if ((state == AssetState::Ready) &&
      (runtimeTexture == kInvalidTextureHandle)) {
    return false;
  }

  record.state = state;
  if (state == AssetState::Ready) {
    record.runtimeTexture = runtimeTexture;
  } else {
    record.runtimeTexture = kInvalidTextureHandle;
  }

  return true;
}

TextureHandle resolve_texture_asset(const AssetDatabase *database,
                                    AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return kInvalidTextureHandle;
  }

  const std::size_t slot = find_texture_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return kInvalidTextureHandle;
  }

  const TextureAssetRecord &record = database->textureAssets[slot];
  if (record.state != AssetState::Ready) {
    return kInvalidTextureHandle;
  }

  return record.runtimeTexture;
}

bool retain_texture_asset(AssetDatabase *database, AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_texture_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return false;
  }

  TextureAssetRecord &record = database->textureAssets[slot];
  ++record.refCount;
  record.requestedResident = true;
  return true;
}

bool release_texture_asset(AssetDatabase *database, AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_texture_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return false;
  }

  TextureAssetRecord &record = database->textureAssets[slot];
  if (record.refCount > 0U) {
    --record.refCount;
  }

  if (record.refCount == 0U) {
    record.requestedResident = false;
  }

  return true;
}

// --- Metadata management ---

namespace {

std::size_t find_metadata_slot(const AssetDatabase *database,
                               AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->metadata.size() : 0U;
  }

  const std::size_t capacity = database->metadata.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->metadataOccupied[slot]) {
      return capacity;
    }
    if (database->metadata[slot].assetId == id) {
      return slot;
    }
  }

  return capacity;
}

std::size_t find_metadata_insert_slot(const AssetDatabase *database,
                                      AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }

  const std::size_t capacity = database->metadata.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->metadataOccupied[slot] ||
        (database->metadata[slot].assetId == id)) {
      return slot;
    }
  }

  return capacity;
}

} // namespace

bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept {
  if ((database == nullptr) || (metadata.assetId == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot =
      find_metadata_insert_slot(database, metadata.assetId);
  if (slot == database->metadata.size()) {
    return false;
  }

  database->metadataOccupied[slot] = true;
  database->metadata[slot] = metadata;
  return true;
}

const AssetMetadata *find_asset_metadata(const AssetDatabase *database,
                                         AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return nullptr;
  }

  const std::size_t slot = find_metadata_slot(database, id);
  if (slot == database->metadata.size()) {
    return nullptr;
  }

  return &database->metadata[slot];
}

bool add_asset_tag(AssetDatabase *database, AssetId id,
                   const char *tag) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) || (tag == nullptr)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(database, id);
  if (slot == database->metadata.size()) {
    return false;
  }

  return asset_metadata_add_tag(&database->metadata[slot], tag);
}

bool asset_has_tag(const AssetDatabase *database, AssetId id,
                   const char *tag) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) || (tag == nullptr)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(database, id);
  if (slot == database->metadata.size()) {
    return false;
  }

  return asset_metadata_has_tag(&database->metadata[slot], tag);
}

std::size_t query_assets_by_tag(const AssetDatabase *database, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept {
  if ((database == nullptr) || (tag == nullptr) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; i < database->metadata.size(); ++i) {
    if (!database->metadataOccupied[i]) {
      continue;
    }
    if (asset_metadata_has_tag(&database->metadata[i], tag)) {
      outIds[count] = database->metadata[i].assetId;
      ++count;
      if (count >= maxIds) {
        break;
      }
    }
  }
  return count;
}

std::size_t query_assets_by_type(const AssetDatabase *database,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept {
  if ((database == nullptr) || (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; i < database->metadata.size(); ++i) {
    if (!database->metadataOccupied[i]) {
      continue;
    }
    if (database->metadata[i].typeTag == typeTag) {
      outIds[count] = database->metadata[i].assetId;
      ++count;
      if (count >= maxIds) {
        break;
      }
    }
  }
  return count;
}

} // namespace engine::renderer
