// Implements asset database behavior for the Engine renderer system.

#include "engine/renderer/asset_database.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "engine/core/hash.h"
#include "engine/core/logging.h"

namespace engine::renderer {

void advance_asset_database_frame(AssetDatabase *database) noexcept {
  if (database != nullptr) {
    ++database->currentFrame;
  }
}

namespace {

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

/// Linear-probe lookup; a never-used slot terminates the probe chain while
/// tombstones keep it alive. Returns capacity when the id is absent.
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

/// Unregisters the asset record; refused while it is still referenced or
/// still owns a GPU mesh.
bool unregister_mesh_asset(AssetDatabase *database, AssetId id) noexcept {
  const std::size_t slot = find_mesh_asset_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  const MeshAssetRecord &record = database->meshAssets[slot];
  if ((record.refCount > 0U) || (record.runtimeMesh != kInvalidMeshHandle)) {
    return false;
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
  record.pinned = true;
  write_source_path(&record.sourcePath, sourcePath);
  return true;
}

/// Marks a mesh asset as requested and loading without queuing a sync load.
bool request_mesh_asset_streaming_load(AssetDatabase *database, AssetId id,
                                       const char *sourcePath) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }
  if ((sourcePath != nullptr) &&
      (std::strlen(sourcePath) >= sizeof(MeshAssetRecord::sourcePath))) {
    core::log_message(core::LogLevel::Error, "assets",
                      "streaming load request rejected: source path too long");
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

/// Sets the mesh asset state; a Ready transition stamps lastAccessFrame so
/// a fresh upload gets a full eviction-hysteresis window even before its
/// first draw resolves it.
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
    record.lastAccessFrame.store(database->currentFrame,
                                 std::memory_order_relaxed);
  } else {
    record.runtimeMesh = kInvalidMeshHandle;
  }

  return true;
}

bool set_mesh_asset_size(AssetDatabase *database, AssetId id,
                         std::uint64_t sizeBytes) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_mesh_asset_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  database->meshAssets[slot].sizeBytes = sizeBytes;
  return true;
}

/// Clears requestedResident on the coldest assets until resident bytes fit
/// the budget — declarative eviction: the asset manager's residency sync
/// sees the cleared flag and unloads the GPU mesh on its next pass.
std::size_t evict_mesh_assets_over_budget(AssetDatabase *database,
                                          std::uint64_t budgetBytes) noexcept {
  if (database == nullptr) {
    return 0U;
  }

  std::uint64_t residentBytes = 0ULL;
  for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
    const MeshAssetRecord &record = database->meshAssets[i];
    if (database->occupied[i] && (record.state == AssetState::Ready) &&
        record.requestedResident) {
      residentBytes += record.sizeBytes;
    }
  }

  std::size_t evicted = 0U;
  while (residentBytes > budgetBytes) {
    std::size_t coldestSlot = database->meshAssets.size();
    std::uint64_t coldestFrame = 0ULL;
    for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
      const MeshAssetRecord &record = database->meshAssets[i];
      if (!database->occupied[i] || (record.state != AssetState::Ready) ||
          !record.requestedResident || record.pinned ||
          (record.refCount > 1U) || (record.sizeBytes == 0ULL)) {
        continue;
      }
      const std::uint64_t lastAccess =
          record.lastAccessFrame.load(std::memory_order_relaxed);
      if ((lastAccess + kMeshEvictionMinAgeFrames) > database->currentFrame) {
        continue;
      }
      if ((coldestSlot == database->meshAssets.size()) ||
          (lastAccess < coldestFrame)) {
        coldestSlot = i;
        coldestFrame = lastAccess;
      }
    }

    if (coldestSlot == database->meshAssets.size()) {
      break;
    }

    MeshAssetRecord &record = database->meshAssets[coldestSlot];
    record.requestedResident = false;
    residentBytes -= record.sizeBytes;
    ++evicted;
  }

  return evicted;
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

  record.lastAccessFrame.store(database->currentFrame,
                               std::memory_order_relaxed);
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
    database->meshTombstoned[i] = false;
    database->meshAssets[i] = MeshAssetRecord{};
  }

  for (std::size_t i = 0U; i < database->textureAssets.size(); ++i) {
    database->textureOccupied[i] = false;
    database->textureAssets[i] = TextureAssetRecord{};
  }

  for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
    database->materialOccupied[i] = false;
    database->materialAssets[i] = MaterialAssetRecord{};
  }

  content::clear_metadata_store(&database->metadataStore);
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

/// Returns the occupied material slot for an id, or capacity when absent.
std::size_t find_material_slot(const AssetDatabase *database,
                               AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->materialAssets.size() : 0U;
  }

  const std::size_t capacity = database->materialAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->materialOccupied[slot]) {
      return capacity;
    }
    if (database->materialAssets[slot].id == id) {
      return slot;
    }
  }

  return capacity;
}

/// Finds the id's material slot or the first free one for insertion.
std::size_t find_material_insert_slot(const AssetDatabase *database,
                                      AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }

  const std::size_t capacity = database->materialAssets.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!database->materialOccupied[slot] ||
        (database->materialAssets[slot].id == id)) {
      return slot;
    }
  }

  return capacity;
}

} // namespace

bool register_material_asset(AssetDatabase *database, AssetId id,
                             const char *sourcePath,
                             const Material &params) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_material_insert_slot(database, id);
  if (slot == database->materialAssets.size()) {
    return false;
  }

  database->materialOccupied[slot] = true;
  MaterialAssetRecord &record = database->materialAssets[slot];
  record.id = id;
  record.params = params;
  record.state = AssetState::Ready;
  write_source_path(&record.sourcePath, sourcePath);
  return true;
}

const Material *find_material_params(const AssetDatabase *database,
                                     AssetId id) noexcept {
  const std::size_t slot = find_material_slot(database, id);
  if ((database == nullptr) || (slot == database->materialAssets.size()) ||
      (database->materialAssets[slot].state != AssetState::Ready)) {
    return nullptr;
  }

  return &database->materialAssets[slot].params;
}

AssetState material_asset_state(const AssetDatabase *database,
                                AssetId id) noexcept {
  const std::size_t slot = find_material_slot(database, id);
  if ((database == nullptr) || (slot == database->materialAssets.size())) {
    return AssetState::Unloaded;
  }

  return database->materialAssets[slot].state;
}

bool set_material_texture_slots(AssetDatabase *database, AssetId id,
                                const MaterialTextureSlots &slots) noexcept {
  const std::size_t slot = find_material_slot(database, id);
  if ((database == nullptr) || (slot == database->materialAssets.size())) {
    return false;
  }

  database->materialAssets[slot].textureSlots = slots;
  return true;
}

const MaterialTextureSlots *
find_material_texture_slots(const AssetDatabase *database,
                            AssetId id) noexcept {
  const std::size_t slot = find_material_slot(database, id);
  if ((database == nullptr) || (slot == database->materialAssets.size())) {
    return nullptr;
  }

  return &database->materialAssets[slot].textureSlots;
}

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

bool register_texture_asset_failed(AssetDatabase *database, AssetId id,
                                   const char *sourcePath) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_texture_insert_slot(database, id);
  if (slot == database->textureAssets.size()) {
    return false;
  }

  database->textureOccupied[slot] = true;
  TextureAssetRecord &record = database->textureAssets[slot];
  record.id = id;
  record.runtimeTexture = kInvalidTextureHandle;
  record.state = AssetState::Failed;
  record.requestedResident = false;
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

// --- Metadata management (#171 C2): thin delegators into the
// content-owned MetadataStore embedded in this database. ---

bool register_asset_metadata(AssetDatabase *database,
                             const AssetMetadata &metadata) noexcept {
  return (database != nullptr) &&
         content::register_asset_metadata(&database->metadataStore, metadata);
}

/// Finds the matching object or resource for asset metadata.
const AssetMetadata *find_asset_metadata(const AssetDatabase *database,
                                         AssetId id) noexcept {
  return (database != nullptr)
             ? content::find_asset_metadata(&database->metadataStore, id)
             : nullptr;
}

/// Adds a tag to the id's metadata; false when unknown or tags full.
bool add_asset_tag(AssetDatabase *database, AssetId id,
                   const char *tag) noexcept {
  return (database != nullptr) &&
         content::add_asset_tag(&database->metadataStore, id, tag);
}

/// True when the id's metadata carries the tag.
bool asset_has_tag(const AssetDatabase *database, AssetId id,
                   const char *tag) noexcept {
  return (database != nullptr) &&
         content::asset_has_tag(&database->metadataStore, id, tag);
}

/// Collects up to maxIds ids carrying the tag; returns the count.
std::size_t query_assets_by_tag(const AssetDatabase *database, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept {
  return (database != nullptr)
             ? content::query_assets_by_tag(&database->metadataStore, tag,
                                            outIds, maxIds)
             : 0U;
}

/// Collects up to maxIds ids of the given type; returns the count.
std::size_t query_assets_by_type(const AssetDatabase *database,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept {
  return (database != nullptr)
             ? content::query_assets_by_type(&database->metadataStore, typeTag,
                                             outIds, maxIds)
             : 0U;
}

/// Copies up to maxIds direct dependencies of the id; returns the count.
std::size_t get_dependencies(const AssetDatabase *database, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept {
  return (database != nullptr)
             ? content::get_dependencies(&database->metadataStore, id, outIds,
                                         maxIds)
             : 0U;
}

/// Records a directed dependency edge id -> depId; false when full.
bool add_asset_dependency(AssetDatabase *database, AssetId id,
                          AssetId depId) noexcept {
  return (database != nullptr) &&
         content::add_asset_dependency(&database->metadataStore, id, depId);
}

namespace {

/// Bridges the renderer callback shape (which receives the database) onto
/// the content-generic dependency walk.
struct DepLoadTrampoline final {
  AssetDatabase *database = nullptr;
  bool (*callback)(AssetDatabase *db, AssetId id, void *userData) = nullptr;
  void *userData = nullptr;
};

bool dep_load_trampoline(AssetId id, void *userData) noexcept {
  auto *bridge = static_cast<DepLoadTrampoline *>(userData);
  if (bridge->callback == nullptr) {
    return true;
  }
  return bridge->callback(bridge->database, id, bridge->userData);
}

} // namespace

/// Loads the requested resource for with dependencies.
bool load_with_dependencies(AssetDatabase *database, AssetId rootId,
                            bool (*loadCallback)(AssetDatabase *db, AssetId id,
                                                 void *userData),
                            void *userData) noexcept {
  if (database == nullptr) {
    return false;
  }

  DepLoadTrampoline bridge{database, loadCallback, userData};
  return content::load_with_dependencies(&database->metadataStore, rootId,
                                         &dep_load_trampoline, &bridge);
}

} // namespace engine::renderer
