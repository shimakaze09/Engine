// Implements asset database behavior for the Engine renderer system.

#include "engine/renderer/asset_database.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "engine/core/hash.h"

namespace engine::renderer {

/// Handles advance asset database frame.
void advance_asset_database_frame(AssetDatabase *database) noexcept {
  if (database != nullptr) {
    ++database->currentFrame;
  }
}

namespace {

/// Handles hashed slot.
std::size_t hashed_slot(AssetId id, std::size_t capacity) noexcept {
  if ((capacity == 0U) || (id == kInvalidAssetId)) {
    return 0U;
  }

  return static_cast<std::size_t>(id) % capacity;
}

/// Finds the matching object or resource for mesh asset slot.
std::size_t find_mesh_asset_slot(const AssetDatabase *database,
                                 AssetId id) noexcept {
  return find_mesh_asset_record_slot(database, id);
}

/// Writes source path data.
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

/// Handles make asset id from path.
AssetId make_asset_id_from_path(const char *path) noexcept {
  if (path == nullptr) {
    return kInvalidAssetId;
  }

  std::uint64_t hash = core::kFnv1a64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(path);
       *cursor != 0U; ++cursor) {
    // Canonicalize separators so the same asset hashes identically on every
    // platform.
    const unsigned char ch = (*cursor == static_cast<unsigned char>('\\'))
                                 ? static_cast<unsigned char>('/')
                                 : *cursor;
    hash = core::fnv1a_64_append(hash, static_cast<std::uint8_t>(ch));
  }

  if (hash == kInvalidAssetId) {
    hash = 1ULL;
  }

  return hash;
}

/// Handles make asset id from file.
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

  std::uint64_t hash = core::kFnv1a64Offset;
  unsigned char buffer[4096] = {};
  while (true) {
    const std::size_t bytesRead = std::fread(buffer, 1U, sizeof(buffer), file);
    if (bytesRead == 0U) {
      break;
    }
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash = core::fnv1a_64_append(hash, buffer[i]);
    }
  }

  std::fclose(file);
  if (hash == kInvalidAssetId) {
    hash = 1ULL;
  }
  return hash;
}

/// Handles register mesh asset.
std::size_t find_mesh_asset_record_slot(const AssetDatabase *database,
                                        AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->meshAssets.size() : 0U;
  }

  const std::size_t capacity = database->meshAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (database->occupied[slot]) {
      if (database->meshAssets[slot].id == id) {
        return slot;
      }
    } else if (!database->meshTombstoned[slot]) {
      // A never-used slot terminates the probe chain; tombstones do not.
      return database->meshAssets.size();
    }
  }

  return database->meshAssets.size();
}

std::size_t claim_mesh_asset_record_slot(AssetDatabase *database,
                                         AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->meshAssets.size() : 0U;
  }

  const std::size_t capacity = database->meshAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  std::size_t tombstone = capacity;
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (database->occupied[slot]) {
      if (database->meshAssets[slot].id == id) {
        return slot;
      }
      continue;
    }

    if (database->meshTombstoned[slot]) {
      if (tombstone == capacity) {
        tombstone = slot;
      }
      continue;
    }

    const std::size_t target = (tombstone != capacity) ? tombstone : slot;
    database->occupied[target] = true;
    database->meshTombstoned[target] = false;
    database->meshAssets[target] = MeshAssetRecord{};
    database->meshAssets[target].id = id;
    return target;
  }

  if (tombstone != capacity) {
    database->occupied[tombstone] = true;
    database->meshTombstoned[tombstone] = false;
    database->meshAssets[tombstone] = MeshAssetRecord{};
    database->meshAssets[tombstone].id = id;
    return tombstone;
  }

  return database->meshAssets.size();
}

bool unregister_mesh_asset(AssetDatabase *database, AssetId id) noexcept {
  const std::size_t slot = find_mesh_asset_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  const MeshAssetRecord &record = database->meshAssets[slot];
  if ((record.refCount > 0U) || (record.runtimeMesh != kInvalidMeshHandle)) {
    return false; // Still referenced or still owns a GPU mesh.
  }

  database->occupied[slot] = false;
  database->meshTombstoned[slot] = true;
  database->meshAssets[slot] = MeshAssetRecord{};
  return true;
}

bool register_mesh_asset(AssetDatabase *database, AssetId id,
                         const char *sourcePath,
                         MeshHandle runtimeMesh) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) ||
      (runtimeMesh == kInvalidMeshHandle)) {
    return false;
  }

  const std::size_t slot = claim_mesh_asset_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  record.id = id;
  record.runtimeMesh = runtimeMesh;
  record.refCount = (record.refCount == 0U) ? 1U : record.refCount;
  record.state = AssetState::Ready;
  record.requestedResident = true;
  write_source_path(&record.sourcePath, sourcePath);
  return true;
}

/// Marks a mesh asset as requested and loading without queuing a sync load.
bool request_mesh_asset_streaming_load(AssetDatabase *database, AssetId id,
                                       const char *sourcePath) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = claim_mesh_asset_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  if ((sourcePath != nullptr) && (sourcePath[0] != '\0')) {
    write_source_path(&record.sourcePath, sourcePath);
  }
  record.refCount = (record.refCount == 0U) ? 1U : record.refCount;
  record.requestedResident = true;
  if (record.state != AssetState::Ready) {
    record.runtimeMesh = kInvalidMeshHandle;
    record.state = AssetState::Loading;
  }
  return true;
}

/// Handles mesh asset state.
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

/// Sets the requested value for mesh asset state.
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

/// Handles mesh asset requested resident.
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

/// Handles resolve mesh asset.
MeshHandle resolve_mesh_asset(AssetDatabase *database, AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return kInvalidMeshHandle;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return kInvalidMeshHandle;
  }

  MeshAssetRecord &record = database->meshAssets[slot];
  if (record.state != AssetState::Ready) {
    return kInvalidMeshHandle;
  }

  record.lastAccessFrame = database->currentFrame;
  return record.runtimeMesh;
}

/// Handles retain mesh asset.
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

/// Handles release mesh asset.
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

/// Handles clear asset database.
void clear_asset_database(AssetDatabase *database) noexcept {
  if (database == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
    database->occupied[i] = false;
    database->meshTombstoned[i] = false;
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

/// Finds the matching object or resource for texture slot.
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

/// Finds the matching object or resource for texture insert slot.
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

/// Handles register texture asset.
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

/// Handles texture asset state.
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

/// Sets the requested value for texture asset state.
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

/// Handles resolve texture asset.
TextureHandle resolve_texture_asset(AssetDatabase *database,
                                    AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return kInvalidTextureHandle;
  }

  const std::size_t slot = find_texture_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return kInvalidTextureHandle;
  }

  TextureAssetRecord &record = database->textureAssets[slot];
  if (record.state != AssetState::Ready) {
    return kInvalidTextureHandle;
  }

  record.lastAccessFrame = database->currentFrame;
  return record.runtimeTexture;
}

/// Handles retain texture asset.
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

/// Handles release texture asset.
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

/// Finds the matching object or resource for metadata slot.
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

/// Finds the matching object or resource for metadata insert slot.
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

/// Handles register asset metadata.
bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept {
  if ((database == nullptr) || (metadata.assetId == kInvalidAssetId) ||
      (metadata.tagCount > AssetMetadata::kMaxTags) ||
      (metadata.dependencyCount > AssetMetadata::kMaxDependencies)) {
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

/// Finds the matching object or resource for asset metadata.
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

/// Adds a value or component to the target system for asset tag.
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

/// Handles asset has tag.
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

/// Handles query assets by tag.
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

/// Handles query assets by type.
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

// --- Dependency management ---

std::size_t get_dependencies(const AssetDatabase *database, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  const AssetMetadata *meta = find_asset_metadata(database, id);
  if (meta == nullptr) {
    return 0U;
  }

  const std::size_t count =
      (meta->dependencyCount < maxIds) ? meta->dependencyCount : maxIds;
  for (std::size_t i = 0U; i < count; ++i) {
    outIds[i] = meta->dependencies[i];
  }
  return count;
}

/// Adds a value or component to the target system for asset dependency.
bool add_asset_dependency(AssetDatabase *database, AssetId id,
                          AssetId depId) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId) ||
      (depId == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(database, id);
  if (slot == database->metadata.size()) {
    return false;
  }

  return asset_metadata_add_dependency(&database->metadata[slot], depId);
}

namespace {

/// Loads the requested resource for with deps recursive.
bool load_with_deps_recursive(AssetDatabase *database, AssetId id,
                              bool (*loadCallback)(AssetDatabase *db,
                                                   AssetId id, void *userData),
                              void *userData, AssetId *visitStack,
                              std::size_t visitDepth, std::size_t maxVisitDepth,
                              AssetId *loadedSet, std::size_t *loadedCount,
                              std::size_t maxLoaded) noexcept {
  // Cycle detection: check if id is already in the visit stack.
  for (std::size_t i = 0U; i < visitDepth; ++i) {
    if (visitStack[i] == id) {
      std::fprintf(stderr,
                   "error: circular dependency detected for asset %016llx\n",
                   static_cast<unsigned long long>(id));
      return false;
    }
  }

  if (visitDepth >= maxVisitDepth) {
    std::fprintf(stderr,
                 "error: dependency chain exceeds maximum depth for asset "
                 "%016llx\n",
                 static_cast<unsigned long long>(id));
    return false;
  }

  // Check if already loaded in this session.
  for (std::size_t i = 0U; i < *loadedCount; ++i) {
    if (loadedSet[i] == id) {
      return true; // Already loaded, skip.
    }
  }

  // Push this asset onto the visit stack.
  visitStack[visitDepth] = id;

  // Load dependencies first.
  const AssetMetadata *meta = find_asset_metadata(database, id);
  if (meta != nullptr) {
    for (std::size_t i = 0U; i < meta->dependencyCount; ++i) {
      const AssetId depId = meta->dependencies[i];
      if (depId == kInvalidAssetId) {
        continue;
      }

      if (!load_with_deps_recursive(database, depId, loadCallback, userData,
                                    visitStack, visitDepth + 1U, maxVisitDepth,
                                    loadedSet, loadedCount, maxLoaded)) {
        return false;
      }
    }
  }

  // Now load the asset itself (after all deps are loaded).
  if (loadCallback != nullptr) {
    if (!loadCallback(database, id, userData)) {
      return false;
    }
  }

  // Mark as loaded.
  if (*loadedCount < maxLoaded) {
    loadedSet[*loadedCount] = id;
    ++(*loadedCount);
  }

  return true;
}

} // namespace

/// Loads the requested resource for with dependencies.
bool load_with_dependencies(AssetDatabase *database, AssetId rootId,
                            bool (*loadCallback)(AssetDatabase *db, AssetId id,
                                                 void *userData),
                            void *userData) noexcept {
  if ((database == nullptr) || (rootId == kInvalidAssetId)) {
    return false;
  }

  constexpr std::size_t kMaxDepth = 64U;
  constexpr std::size_t kMaxLoaded = 256U;
  AssetId visitStack[kMaxDepth] = {};
  AssetId loadedSet[kMaxLoaded] = {};
  std::size_t loadedCount = 0U;

  return load_with_deps_recursive(database, rootId, loadCallback, userData,
                                  visitStack, 0U, kMaxDepth, loadedSet,
                                  &loadedCount, kMaxLoaded);
}

} // namespace engine::renderer
