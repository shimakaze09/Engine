// Implements the asset catalog's table and the asset-id constructors: the
// fixed-slot record store, its lookups by id, identity and path, the
// write rules and the change generation, and the tag, dependency and query
// logic. The mount walk that fills it lives in asset_catalog.cpp.

#include "engine/content/asset_catalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/content/asset_identity.h"
#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/thread_affinity.h"
#include "engine/core/vfs.h"

namespace engine::content {

namespace {

/// Open-addressing base slot for an id.
std::size_t hashed_slot(AssetId id, std::size_t capacity) noexcept {
  if ((capacity == 0U) || (id == kInvalidAssetId)) {
    return 0U;
  }

  return static_cast<std::size_t>(id) % capacity;
}

} // namespace

/// Resets every slot back to the empty state.
void clear_asset_catalog(AssetCatalog *catalog) noexcept {
  if (catalog == nullptr) {
    return;
  }
  ENGINE_ASSERT_MAIN_THREAD();

  for (std::size_t i = 0U; i < catalog->entries.size(); ++i) {
    catalog->occupied[i] = false;
    catalog->entries[i] = AssetMetadata{};
    catalog->reloadGenerations[i] = 0U;
  }
  ++catalog->generation;
}

// --- Metadata management ---

namespace {

/// Finds the matching object or resource for metadata slot.
std::size_t find_metadata_slot(const AssetCatalog *catalog,
                               AssetId id) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId)) {
    return catalog != nullptr ? catalog->entries.size() : 0U;
  }

  const std::size_t capacity = catalog->entries.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!catalog->occupied[slot]) {
      return capacity;
    }
    if (catalog->entries[slot].assetId == id) {
      return slot;
    }
  }

  return capacity;
}

/// Finds the matching object or resource for metadata insert slot.
std::size_t find_metadata_insert_slot(const AssetCatalog *catalog,
                                      AssetId id) noexcept {
  if (catalog == nullptr) {
    return 0U;
  }

  const std::size_t capacity = catalog->entries.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!catalog->occupied[slot] || (catalog->entries[slot].assetId == id)) {
      return slot;
    }
  }

  return capacity;
}

} // namespace

bool register_asset_metadata(AssetCatalog *catalog,
                             const AssetMetadata &metadata) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  if ((catalog == nullptr) || (metadata.assetId == kInvalidAssetId) ||
      (metadata.tagCount > AssetMetadata::kMaxTags) ||
      (metadata.dependencyCount > AssetMetadata::kMaxDependencies)) {
    return false;
  }

  // The fixed-width strings are compared with strcmp downstream
  // (asset_metadata_has_tag), so a caller-filled array without a
  // terminator is refused here rather than read past its end there.
  if (std::memchr(metadata.filePath.data(), '\0', metadata.filePath.size()) ==
      nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < metadata.tagCount; ++i) {
    if (std::memchr(metadata.tags[i].data(), '\0',
                    AssetMetadata::kMaxTagLength) == nullptr) {
      return false;
    }
  }

  const std::size_t slot = find_metadata_insert_slot(catalog, metadata.assetId);
  if (slot == catalog->entries.size()) {
    return false;
  }

  if (!catalog->occupied[slot]) {
    catalog->reloadGenerations[slot] = 0U;
  }
  catalog->occupied[slot] = true;
  catalog->entries[slot] = metadata;
  ++catalog->generation;
  return true;
}

std::uint32_t asset_reload_generation(const AssetCatalog *catalog,
                                      AssetId id) noexcept {
  const std::size_t slot = find_metadata_slot(catalog, id);
  if ((catalog == nullptr) || (slot == catalog->entries.size())) {
    return 0U;
  }
  return catalog->reloadGenerations[slot];
}

bool note_asset_reloaded(AssetCatalog *catalog, AssetId id) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  const std::size_t slot = find_metadata_slot(catalog, id);
  if ((catalog == nullptr) || (slot == catalog->entries.size())) {
    return false;
  }
  ++catalog->reloadGenerations[slot];
  ++catalog->generation;
  return true;
}

CatalogInsert
register_asset_metadata_if_absent(AssetCatalog *catalog,
                                  const AssetMetadata &metadata) noexcept {
  if (find_asset_metadata(catalog, metadata.assetId) != nullptr) {
    return CatalogInsert::AlreadyKnown;
  }
  return register_asset_metadata(catalog, metadata) ? CatalogInsert::Registered
                                                    : CatalogInsert::Refused;
}

bool can_register_asset_metadata(const AssetCatalog *catalog,
                                 AssetId id) noexcept {
  return (catalog != nullptr) && (id != kInvalidAssetId) &&
         (find_metadata_insert_slot(catalog, id) != catalog->entries.size());
}

/// Finds the matching object or resource for asset metadata.
const AssetMetadata *find_asset_metadata(const AssetCatalog *catalog,
                                         AssetId id) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId)) {
    return nullptr;
  }

  const std::size_t slot = find_metadata_slot(catalog, id);
  if (slot == catalog->entries.size()) {
    return nullptr;
  }

  return &catalog->entries[slot];
}

const AssetMetadata *
find_asset_metadata_by_path(const AssetCatalog *catalog,
                            const char *virtualPath) noexcept {
  char wanted[sizeof(AssetMetadata::filePath)] = {};
  if ((catalog == nullptr) ||
      !core::canonical_virtual_path(virtualPath, wanted, sizeof(wanted))) {
    return nullptr;
  }
  const AssetMetadata *record =
      find_asset_metadata(catalog, make_asset_id_from_path(wanted));
  char stored[sizeof(AssetMetadata::filePath)] = {};
  if ((record == nullptr) ||
      !core::canonical_virtual_path(record->filePath.data(), stored,
                                    sizeof(stored)) ||
      (std::strcmp(stored, wanted) != 0)) {
    return nullptr;
  }
  return record;
}

const AssetMetadata *find_asset_metadata_by_ref(const AssetCatalog *catalog,
                                                const AssetRef &ref) noexcept {
  if ((catalog == nullptr) || !asset_ref_is_valid(ref)) {
    return nullptr;
  }
  for (std::size_t slot = 0U; slot < catalog->entries.size(); ++slot) {
    if (!catalog->occupied[slot]) {
      continue;
    }
    if (catalog->entries[slot].ref == ref) {
      return &catalog->entries[slot];
    }
  }
  return nullptr;
}

std::size_t find_duplicate_guid_records(const AssetCatalog *catalog,
                                        const AssetMetadata **outRecords,
                                        std::size_t capacity) noexcept {
  if (catalog == nullptr) {
    return 0U;
  }
  std::size_t found = 0U;
  for (std::size_t slot = 0U; slot < catalog->entries.size(); ++slot) {
    if (!catalog->occupied[slot]) {
      continue;
    }
    const AssetMetadata &record = catalog->entries[slot];
    if (!asset_ref_is_valid(record.ref)) {
      continue;
    }
    // Only a full ref collision is a duplicate: two outputs of one
    // source share its GUID by design and are told apart by local id.
    bool collides = false;
    for (std::size_t other = 0U; other < catalog->entries.size(); ++other) {
      if ((other == slot) || !catalog->occupied[other]) {
        continue;
      }
      if (catalog->entries[other].ref == record.ref) {
        collides = true;
        break;
      }
    }
    if (!collides) {
      continue;
    }
    if ((outRecords != nullptr) && (found < capacity)) {
      outRecords[found] = &record;
    }
    ++found;
  }
  return found;
}

bool add_asset_tag(AssetCatalog *catalog, AssetId id,
                   const char *tag) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId) || (tag == nullptr)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(catalog, id);
  if (slot == catalog->entries.size()) {
    return false;
  }

  ENGINE_ASSERT_MAIN_THREAD();
  AssetMetadata &record = catalog->entries[slot];
  const std::size_t before = record.tagCount;
  if (!asset_metadata_add_tag(&record, tag)) {
    return false;
  }
  if (record.tagCount != before) {
    ++catalog->generation;
  }
  return true;
}

bool asset_has_tag(const AssetCatalog *catalog, AssetId id,
                   const char *tag) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId) || (tag == nullptr)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(catalog, id);
  if (slot == catalog->entries.size()) {
    return false;
  }

  return asset_metadata_has_tag(&catalog->entries[slot], tag);
}

std::size_t query_assets_by_tag(const AssetCatalog *catalog, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept {
  if ((catalog == nullptr) || (tag == nullptr) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; i < catalog->entries.size(); ++i) {
    if (!catalog->occupied[i]) {
      continue;
    }
    if (asset_metadata_has_tag(&catalog->entries[i], tag)) {
      outIds[count] = catalog->entries[i].assetId;
      ++count;
      if (count >= maxIds) {
        break;
      }
    }
  }
  return count;
}

std::size_t query_assets_by_type(const AssetCatalog *catalog,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept {
  if ((catalog == nullptr) || (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; i < catalog->entries.size(); ++i) {
    if (!catalog->occupied[i]) {
      continue;
    }
    if (catalog->entries[i].typeTag == typeTag) {
      outIds[count] = catalog->entries[i].assetId;
      ++count;
      if (count >= maxIds) {
        break;
      }
    }
  }
  return count;
}

// --- Dependency management ---

std::size_t get_dependencies(const AssetCatalog *catalog, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  const AssetMetadata *meta = find_asset_metadata(catalog, id);
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

bool add_asset_dependency(AssetCatalog *catalog, AssetId id,
                          AssetId depId) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId) ||
      (depId == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(catalog, id);
  if (slot == catalog->entries.size()) {
    return false;
  }

  ENGINE_ASSERT_MAIN_THREAD();
  AssetMetadata &record = catalog->entries[slot];
  const std::size_t before = record.dependencyCount;
  if (!asset_metadata_add_dependency(&record, depId)) {
    return false;
  }
  if (record.dependencyCount != before) {
    ++catalog->generation;
  }
  return true;
}

namespace {

/// Longest dependency chain one traversal follows before reporting the
/// graph as too deep.
constexpr std::size_t kMaxDependencyDepth = 64U;

/// Assets whose load callback has already run during one traversal.
/// A registered asset is marked by its table slot, so deduplication holds
/// for every record the catalog can hold; an id the catalog has no record for
/// owns no slot, so those are held in a side list instead.
struct VisitedAssets final {
  static constexpr std::size_t kMaxUnregistered = 256U;
  static constexpr std::size_t kMarkBits = 64U;
  static constexpr std::size_t kMarkWords =
      (AssetCatalog::kMaxMetadata + kMarkBits - 1U) / kMarkBits;

  std::uint64_t slotMarks[kMarkWords] = {};
  AssetId unregistered[kMaxUnregistered] = {};
  std::size_t unregisteredCount = 0U;
};

/// Whether the asset already ran its callback. `slot` is the id's slot in
/// the catalog, or AssetCatalog::kMaxMetadata when the catalog holds no
/// record for it. The side list is scanned either way, because a callback
/// that registers metadata can give an id a slot it did not have when it
/// was first visited.
bool asset_visited(const VisitedAssets &visited, std::size_t slot,
                   AssetId id) noexcept {
  if (slot < AssetCatalog::kMaxMetadata) {
    const std::uint64_t bit = 1ULL << (slot % VisitedAssets::kMarkBits);
    if ((visited.slotMarks[slot / VisitedAssets::kMarkBits] & bit) != 0ULL) {
      return true;
    }
  }

  for (std::size_t i = 0U; i < visited.unregisteredCount; ++i) {
    if (visited.unregistered[i] == id) {
      return true;
    }
  }

  return false;
}

/// Records that the asset ran its callback. False only when an id without
/// a metadata record finds the side list full: the caller reports that
/// instead of continuing, because continuing would load an id shared by
/// several dependents once per dependent.
bool mark_asset_visited(VisitedAssets &visited, std::size_t slot,
                        AssetId id) noexcept {
  if (slot < AssetCatalog::kMaxMetadata) {
    const std::uint64_t bit = 1ULL << (slot % VisitedAssets::kMarkBits);
    visited.slotMarks[slot / VisitedAssets::kMarkBits] |= bit;
    return true;
  }

  if (visited.unregisteredCount >= VisitedAssets::kMaxUnregistered) {
    return false;
  }

  visited.unregistered[visited.unregisteredCount] = id;
  ++visited.unregisteredCount;
  return true;
}

/// State one traversal carries across the recursion: the catalog it walks,
/// the callback and its user data, the ancestor chain cycles are detected
/// against, and the assets already loaded.
struct DependencyTraversal final {
  AssetCatalog *catalog = nullptr;
  bool (*loadCallback)(AssetId id, void *userData) = nullptr;
  void *userData = nullptr;
  AssetId visitStack[kMaxDependencyDepth] = {};
  VisitedAssets visited{};
};

/// Depth-first dependency load: rejects cycles via the visit stack,
/// skips assets already loaded this traversal, and loads every dependency
/// before the asset itself.
bool load_with_deps_recursive(DependencyTraversal &traversal, AssetId id,
                              std::size_t visitDepth) noexcept {
  for (std::size_t i = 0U; i < visitDepth; ++i) {
    if (traversal.visitStack[i] == id) {
      std::fprintf(stderr,
                   "error: circular dependency detected for asset %016llx\n",
                   static_cast<unsigned long long>(id));
      return false;
    }
  }

  if (visitDepth >= kMaxDependencyDepth) {
    std::fprintf(stderr,
                 "error: dependency chain exceeds maximum depth for asset "
                 "%016llx\n",
                 static_cast<unsigned long long>(id));
    return false;
  }

  const std::size_t slot = find_metadata_slot(traversal.catalog, id);
  if (asset_visited(traversal.visited, slot, id)) {
    return true;
  }

  traversal.visitStack[visitDepth] = id;

  if (slot < AssetCatalog::kMaxMetadata) {
    const AssetMetadata &meta = traversal.catalog->entries[slot];
    for (std::size_t i = 0U; i < meta.dependencyCount; ++i) {
      const AssetId depId = meta.dependencies[i];
      if (depId == kInvalidAssetId) {
        continue;
      }

      if (!load_with_deps_recursive(traversal, depId, visitDepth + 1U)) {
        return false;
      }
    }
  }

  if (traversal.loadCallback != nullptr) {
    if (!traversal.loadCallback(id, traversal.userData)) {
      return false;
    }
  }

  if (!mark_asset_visited(traversal.visited, slot, id)) {
    std::fprintf(stderr,
                 "error: dependency traversal reached more than %zu assets "
                 "with no metadata record, at %016llx; register their "
                 "metadata so each loads exactly once\n",
                 VisitedAssets::kMaxUnregistered,
                 static_cast<unsigned long long>(id));
    return false;
  }

  return true;
}

} // namespace

namespace {

/// Whether catalog slot `slot`'s record lists `id` among its dependencies.
bool slot_depends_on(const AssetCatalog &catalog, std::size_t slot,
                     AssetId id) noexcept {
  const AssetMetadata &record = catalog.entries[slot];
  for (std::size_t i = 0U; i < record.dependencyCount; ++i) {
    if (record.dependencies[i] == id) {
      return true;
    }
  }
  return false;
}

} // namespace

std::size_t find_asset_dependents(const AssetCatalog *catalog, AssetId id,
                                  AssetId *outIds,
                                  std::size_t maxIds) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId)) {
    return 0U;
  }
  std::size_t count = 0U;
  for (std::size_t slot = 0U; slot < catalog->entries.size(); ++slot) {
    if (!catalog->occupied[slot] || !slot_depends_on(*catalog, slot, id)) {
      continue;
    }
    if ((outIds != nullptr) && (count < maxIds)) {
      outIds[count] = catalog->entries[slot].assetId;
    }
    ++count;
  }
  return count;
}

std::size_t notify_asset_changed(const AssetCatalog *catalog, AssetId changed,
                                 AssetChangeVisitor visit,
                                 void *userData) noexcept {
  if ((catalog == nullptr) || (changed == kInvalidAssetId) ||
      (visit == nullptr)) {
    return 0U;
  }

  // Every dependent owns a catalog slot, so a slot bitset remembers what
  // was visited, and a queue of slots can never hold more than the table.
  // The changed asset starts the queue by id: it may own no slot at all.
  static_assert(AssetCatalog::kMaxMetadata <= 65536U,
                "a notify queue entry holds a catalog slot in 16 bits");
  VisitedAssets visited{};
  std::array<std::uint16_t, AssetCatalog::kMaxMetadata> queue{};
  std::size_t head = 0U;
  std::size_t tail = 0U;
  const std::size_t changedSlot = find_metadata_slot(catalog, changed);
  if (changedSlot < AssetCatalog::kMaxMetadata) {
    static_cast<void>(mark_asset_visited(visited, changedSlot, changed));
  }

  AssetId cause = changed;
  for (;;) {
    for (std::size_t slot = 0U; slot < catalog->entries.size(); ++slot) {
      if (!catalog->occupied[slot] ||
          asset_visited(visited, slot, catalog->entries[slot].assetId) ||
          !slot_depends_on(*catalog, slot, cause)) {
        continue;
      }
      static_cast<void>(
          mark_asset_visited(visited, slot, catalog->entries[slot].assetId));
      queue[tail++] = static_cast<std::uint16_t>(slot);
      visit(catalog->entries[slot].assetId, cause, userData);
    }
    if (head == tail) {
      break;
    }
    cause = catalog->entries[queue[head++]].assetId;
  }
  return tail;
}

/// Loads the requested resource for with dependencies.
bool load_with_dependencies(AssetCatalog *catalog, AssetId rootId,
                            bool (*loadCallback)(AssetId id, void *userData),
                            void *userData) noexcept {
  if ((catalog == nullptr) || (rootId == kInvalidAssetId)) {
    return false;
  }

  // The traversal's state lives in this frame and is threaded through the
  // recursion by reference, so the visited marks cost one ~3 KB frame per
  // call rather than one per dependency level, and nothing is allocated.
  DependencyTraversal traversal{};
  traversal.catalog = catalog;
  traversal.loadCallback = loadCallback;
  traversal.userData = userData;

  return load_with_deps_recursive(traversal, rootId, 0U);
}

// --- Asset identity constructors ---

/// FNV-1a over the path's canonical virtual spelling, so the id the
/// runtime derives is the id the catalog registered no matter how the
/// reference was written. Hashing the raw path instead gave one asset as
/// many ids as it had spellings: "assets//coin.mesh" resolved to the same
/// file as "assets/coin.mesh" and owned a different id, which a saved
/// reference then failed to resolve.
AssetId make_asset_id_from_path(const char *path) noexcept {
  // One derivation, in PathKey: this is the numeric form of the same key,
  // kept while references written before GUID identity are still read.
  // kInvalidPathKey and kInvalidAssetId are both zero, so a path that
  // names no asset — empty, nothing but separators or "." segments,
  // carrying a "..", or too long to hold whole — maps straight through.
  return make_path_key(path).value;
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
    core::log_message(core::LogLevel::Warning, "assets",
                      "asset id falls back to path hash: file unreadable");
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

  const bool readFailed = std::ferror(file) != 0;
  if (std::fclose(file) != 0) {
    core::log_message(core::LogLevel::Warning, "assets",
                      "asset id hashing: close failed after read");
  }
  if (readFailed) {
    core::log_message(core::LogLevel::Warning, "assets",
                      "asset id falls back to path hash: read error left a "
                      "partial content hash");
    return make_asset_id_from_path(path);
  }
  if (hash == kInvalidAssetId) {
    hash = 1ULL;
  }
  return hash;
}

} // namespace engine::content
