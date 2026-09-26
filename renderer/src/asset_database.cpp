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

namespace {

/// Re-indexes every live record, dropping the tombstones erases left.
void rebuild_mesh_index(AssetDatabase *database) noexcept {
  database->meshIndex.clear();
  for (std::size_t slot = 0U; slot < database->meshAssets.size(); ++slot) {
    if (database->occupied[slot]) {
      static_cast<void>(database->meshIndex.insert(
          database->meshAssets[slot].id, static_cast<std::uint32_t>(slot)));
    }
  }
}

} // namespace

std::size_t find_mesh_asset_record_slot(const AssetDatabase *database,
                                        AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->meshAssets.size() : 0U;
  }

  const std::uint32_t *slot = database->meshIndex.find(id);
  return (slot != nullptr) ? static_cast<std::size_t>(*slot)
                           : database->meshAssets.size();
}

std::size_t claim_mesh_asset_record_slot(AssetDatabase *database,
                                         AssetId id) noexcept {
  if ((database == nullptr) || (id == kInvalidAssetId)) {
    return database != nullptr ? database->meshAssets.size() : 0U;
  }

  const std::size_t existing = find_mesh_asset_record_slot(database, id);
  if (existing != database->meshAssets.size()) {
    return existing;
  }

  for (std::size_t slot = 0U; slot < database->meshAssets.size(); ++slot) {
    if (database->occupied[slot]) {
      continue;
    }
    // The index has twice the records' capacity, so while a record slot is
    // free it has room; the check keeps the two in step regardless.
    if (!database->meshIndex.insert(id, static_cast<std::uint32_t>(slot))) {
      return database->meshAssets.size();
    }
    database->occupied[slot] = true;
    database->meshAssets[slot] = MeshAssetRecord{};
    database->meshAssets[slot].id = id;
    return slot;
  }

  ++database->refusedMeshClaims;
  return database->meshAssets.size();
}

bool mesh_asset_record_releasable(const MeshAssetRecord &record) noexcept {
  return !record.requestedResident && !record.pinned && (record.refCount <= 1U);
}

/// Unregisters the asset record; refused while it is still referenced or
/// still owns a GPU mesh.
bool unregister_mesh_asset(AssetDatabase *database, AssetId id) noexcept {
  if (database == nullptr) {
    return false;
  }
  const std::size_t slot = find_mesh_asset_record_slot(database, id);
  if (slot == database->meshAssets.size()) {
    return false;
  }

  const MeshAssetRecord &record = database->meshAssets[slot];
  if ((record.refCount > 0U) || (record.runtimeMesh != kInvalidMeshHandle)) {
    return false;
  }

  database->occupied[slot] = false;
  database->meshAssets[slot] = MeshAssetRecord{};
  static_cast<void>(database->meshIndex.erase(id));
  if (database->meshIndex.tombstone_count() >
      (AssetDatabase::kMeshIndexCapacity / 4U)) {
    rebuild_mesh_index(database);
  }
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
  // Records free now or already on their way out: a refused claim retries
  // next frame and one of these serves it, so only the shortfall past
  // them is evicted.
  std::size_t comingFree = 0U;
  for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
    const MeshAssetRecord &record = database->meshAssets[i];
    if (!database->occupied[i] || mesh_asset_record_releasable(record)) {
      ++comingFree;
      continue;
    }
    if ((record.state == AssetState::Ready) && record.requestedResident) {
      residentBytes += record.sizeBytes;
    }
  }
  const std::size_t wantedFree = database->refusedMeshClaims;
  database->refusedMeshClaims = 0U;

  std::size_t evicted = 0U;
  while ((residentBytes > budgetBytes) || (comingFree < wantedFree)) {
    // Under byte pressure alone a sizeless record frees nothing worth
    // taking; under record pressure it frees exactly what is short.
    const bool recordPressure = comingFree < wantedFree;
    std::size_t coldestSlot = database->meshAssets.size();
    std::uint64_t coldestFrame = 0ULL;
    for (std::size_t i = 0U; i < database->meshAssets.size(); ++i) {
      const MeshAssetRecord &record = database->meshAssets[i];
      if (!database->occupied[i] || (record.state != AssetState::Ready) ||
          !record.requestedResident || record.pinned ||
          (record.refCount > 1U) ||
          ((record.sizeBytes == 0ULL) && !recordPressure)) {
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
    ++comingFree;
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
    database->meshAssets[i] = MeshAssetRecord{};
  }
  database->meshIndex.clear();
  database->refusedMeshClaims = 0U;

  for (std::size_t i = 0U; i < database->textureAssets.size(); ++i) {
    database->textureOccupied[i] = false;
    database->textureAssets[i] = TextureAssetRecord{};
  }
  database->textureIndex.clear();

  for (std::size_t i = 0U; i < database->materialAssets.size(); ++i) {
    database->materialOccupied[i] = false;
    database->materialAssets[i] = MaterialAssetRecord{};
  }
  database->materialIndex.clear();
}

// --- Texture asset functions ---

namespace {

/// The record slot the index maps `id` to, or `capacity` when absent.
template <typename Index>
std::size_t indexed_slot(const Index &index, AssetId id,
                         std::size_t capacity) noexcept {
  if (id == kInvalidAssetId) {
    return capacity;
  }
  const std::uint32_t *slot = index.find(id);
  return (slot != nullptr) ? static_cast<std::size_t>(*slot) : capacity;
}

/// The id's slot, or else the first free record slot; `occupied.size()`
/// when neither exists. Registration is the only caller, so the scan for a
/// free slot is off every per-frame path.
template <typename Index, std::size_t N>
std::size_t indexed_insert_slot(const Index &index,
                                const std::array<bool, N> &occupied,
                                AssetId id) noexcept {
  if (id == kInvalidAssetId) {
    return N;
  }
  const std::size_t existing = indexed_slot(index, id, N);
  if (existing != N) {
    return existing;
  }
  for (std::size_t slot = 0U; slot < N; ++slot) {
    if (!occupied[slot]) {
      return slot;
    }
  }
  return N;
}

/// Marks a slot claimed for `id` and indexes it. False, with the slot left
/// free, when the index has no room -- it has twice the records' capacity,
/// so this does not happen while a record slot is free.
template <typename Index, std::size_t N>
bool claim_indexed_slot(Index &index, std::array<bool, N> &occupied,
                        std::size_t slot, AssetId id) noexcept {
  if (occupied[slot]) {
    return true;
  }
  if (!index.insert(id, static_cast<std::uint32_t>(slot))) {
    return false;
  }
  occupied[slot] = true;
  return true;
}

std::size_t find_texture_slot(const AssetDatabase *database,
                              AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }
  return indexed_slot(database->textureIndex, id,
                      database->textureAssets.size());
}

std::size_t find_texture_insert_slot(const AssetDatabase *database,
                                     AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }
  return indexed_insert_slot(database->textureIndex, database->textureOccupied,
                             id);
}

std::size_t find_material_slot(const AssetDatabase *database,
                               AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }
  return indexed_slot(database->materialIndex, id,
                      database->materialAssets.size());
}

std::size_t find_material_insert_slot(const AssetDatabase *database,
                                      AssetId id) noexcept {
  if (database == nullptr) {
    return 0U;
  }
  return indexed_insert_slot(database->materialIndex,
                             database->materialOccupied, id);
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

  if (!claim_indexed_slot(database->materialIndex, database->materialOccupied,
                          slot, id)) {
    return false;
  }
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
  database->materialAssets[slot].unregisterableTextureSlots = 0U;
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

bool set_material_overrides(AssetDatabase *database, AssetId id,
                            std::uint16_t overriddenFields) noexcept {
  const std::size_t slot = find_material_slot(database, id);
  if ((database == nullptr) || (slot == database->materialAssets.size())) {
    return false;
  }

  database->materialAssets[slot].overriddenFields =
      overriddenFields & material_field::kAll;
  return true;
}

std::uint16_t material_overrides(const AssetDatabase *database,
                                 AssetId id) noexcept {
  const std::size_t slot = find_material_slot(database, id);
  if ((database == nullptr) || (slot == database->materialAssets.size())) {
    return material_field::kAll;
  }
  return database->materialAssets[slot].overriddenFields;
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

  if (!claim_indexed_slot(database->textureIndex, database->textureOccupied,
                          slot, id)) {
    return false;
  }
  TextureAssetRecord &record = database->textureAssets[slot];
  record.id = id;
  record.runtimeTexture = runtimeTexture;
  record.refCount = (record.refCount == 0U) ? 1U : record.refCount;
  record.state = AssetState::Ready;
  record.requestedResident = true;
  write_source_path(&record.sourcePath, sourcePath);
  return true;
}

bool texture_asset_slot_available(const AssetDatabase *database,
                                  AssetId id) noexcept {
  return (database != nullptr) && (id != kInvalidAssetId) &&
         (find_texture_insert_slot(database, id) !=
          database->textureAssets.size());
}

bool material_asset_slot_available(const AssetDatabase *database,
                                   AssetId id) noexcept {
  return (database != nullptr) && (id != kInvalidAssetId) &&
         (find_material_insert_slot(database, id) !=
          database->materialAssets.size());
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

  if (!claim_indexed_slot(database->textureIndex, database->textureOccupied,
                          slot, id)) {
    return false;
  }
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

} // namespace engine::renderer
