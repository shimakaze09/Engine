// Implements the asset catalog's table and the asset-id constructors: the
// record store, which grows in pages up to a configured limit, its lookups
// by id, identity and path, the write rules and the change generation, and
// the tag, dependency and query logic. The mount walk that fills it lives
// in asset_catalog.cpp.

#include "engine/content/asset_catalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/content/asset_identity.h"
#include "engine/core/hash.h"
#include "engine/core/logging.h"
#include "engine/core/mem_tracker.h"
#include "engine/core/thread_affinity.h"
#include "engine/core/vfs.h"

namespace engine::content {

/// One page of records. A record never moves once written, so a pointer
/// to it stays good for as long as the catalog is not cleared.
struct AssetCatalog::Page final {
  std::array<AssetMetadata, kPageRecords> records =
      std::array<AssetMetadata, kPageRecords>();
  std::array<std::uint32_t, kPageRecords> reloadGenerations{};
};

AssetCatalog::~AssetCatalog() noexcept {
  for (Page *&page : pages) {
    if (page != nullptr) {
      core::mem_tracker_free(core::MemTag::Assets, sizeof(Page));
    }
    delete page;
    page = nullptr;
  }
  if (index != nullptr) {
    core::mem_tracker_free(core::MemTag::Assets,
                           indexCapacity * sizeof(std::uint32_t));
  }
  delete[] index;
  index = nullptr;
}

namespace {

/// Returned by find_record for an id the catalog holds no record for.
constexpr std::size_t kNoRecord = static_cast<std::size_t>(-1);
/// The id index's size when it is first needed; it doubles from here.
constexpr std::size_t kInitialIndexCapacity = 1024U;

AssetMetadata &record_at(AssetCatalog &catalog, std::size_t record) noexcept {
  return catalog.pages[record / AssetCatalog::kPageRecords]
      ->records[record % AssetCatalog::kPageRecords];
}

const AssetMetadata &record_at(const AssetCatalog &catalog,
                               std::size_t record) noexcept {
  return catalog.pages[record / AssetCatalog::kPageRecords]
      ->records[record % AssetCatalog::kPageRecords];
}

std::uint32_t &reload_generation_at(AssetCatalog &catalog,
                                    std::size_t record) noexcept {
  return catalog.pages[record / AssetCatalog::kPageRecords]
      ->reloadGenerations[record % AssetCatalog::kPageRecords];
}

/// The index slot an id's probe starts at; the capacity is a power of two.
std::size_t index_home(AssetId id, std::size_t capacity) noexcept {
  return static_cast<std::size_t>(id) & (capacity - 1U);
}

/// The record `id` has, or kNoRecord. The index stores record numbers
/// plus one, zero marking an empty slot, and is never more than half full,
/// so a probe ends quickly at a match or an empty slot.
std::size_t find_record(const AssetCatalog *catalog, AssetId id) noexcept {
  if ((catalog == nullptr) || (id == kInvalidAssetId) ||
      (catalog->index == nullptr)) {
    return kNoRecord;
  }
  const std::size_t capacity = catalog->indexCapacity;
  for (std::size_t probe = 0U, slot = index_home(id, capacity);
       probe < capacity; ++probe, slot = (slot + 1U) & (capacity - 1U)) {
    const std::uint32_t entry = catalog->index[slot];
    if (entry == 0U) {
      return kNoRecord;
    }
    if (record_at(*catalog, entry - 1U).assetId == id) {
      return entry - 1U;
    }
  }
  return kNoRecord;
}

/// Adds `record` to an index with room for it.
void index_insert(std::uint32_t *index, std::size_t capacity, AssetId id,
                  std::size_t record) noexcept {
  std::size_t slot = index_home(id, capacity);
  while (index[slot] != 0U) {
    slot = (slot + 1U) & (capacity - 1U);
  }
  index[slot] = static_cast<std::uint32_t>(record + 1U);
}

/// Makes room for one more record: its page and an index slot, keeping the
/// index at most half full, and reports what it allocates under the
/// Assets memory tag. False, with nothing changed, when an allocation
/// fails.
bool reserve_record(AssetCatalog *catalog) noexcept {
  const std::size_t record = catalog->recordCount;
  AssetCatalog::Page *&page =
      catalog->pages[record / AssetCatalog::kPageRecords];
  if (page == nullptr) {
    page = new (std::nothrow) AssetCatalog::Page();
    if (page == nullptr) {
      return false;
    }
    core::mem_tracker_alloc(core::MemTag::Assets, sizeof(AssetCatalog::Page));
  }
  if (((record + 1U) * 2U) <= catalog->indexCapacity) {
    return true;
  }
  const std::size_t capacity = (catalog->indexCapacity == 0U)
                                   ? kInitialIndexCapacity
                                   : (catalog->indexCapacity * 2U);
  std::uint32_t *grown = new (std::nothrow) std::uint32_t[capacity]();
  if (grown == nullptr) {
    return false;
  }
  for (std::size_t i = 0U; i < record; ++i) {
    index_insert(grown, capacity, record_at(*catalog, i).assetId, i);
  }
  core::mem_tracker_alloc(core::MemTag::Assets,
                          capacity * sizeof(std::uint32_t));
  if (catalog->index != nullptr) {
    core::mem_tracker_free(core::MemTag::Assets,
                           catalog->indexCapacity * sizeof(std::uint32_t));
  }
  delete[] catalog->index;
  catalog->index = grown;
  catalog->indexCapacity = capacity;
  return true;
}

} // namespace

/// Resets every record back to the empty state. Pages and the index keep
/// their memory for the records that come next.
void clear_asset_catalog(AssetCatalog *catalog) noexcept {
  if (catalog == nullptr) {
    return;
  }
  ENGINE_ASSERT_MAIN_THREAD();

  for (std::size_t i = 0U; i < catalog->recordCount; ++i) {
    record_at(*catalog, i) = AssetMetadata{};
    reload_generation_at(*catalog, i) = 0U;
  }
  catalog->recordCount = 0U;
  if (catalog->index != nullptr) {
    std::memset(catalog->index, 0,
                catalog->indexCapacity * sizeof(catalog->index[0]));
  }
  ++catalog->generation;
}

std::size_t asset_catalog_record_count(const AssetCatalog *catalog) noexcept {
  return (catalog != nullptr) ? catalog->recordCount : 0U;
}

const AssetMetadata *asset_catalog_record(const AssetCatalog *catalog,
                                          std::size_t record) noexcept {
  if ((catalog == nullptr) || (record >= catalog->recordCount)) {
    return nullptr;
  }
  return &record_at(*catalog, record);
}

// --- Metadata management ---

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

  const std::size_t existing = find_record(catalog, metadata.assetId);
  if (existing != kNoRecord) {
    record_at(*catalog, existing) = metadata;
    ++catalog->generation;
    return true;
  }
  if (!can_register_asset_metadata(catalog, metadata.assetId) ||
      !reserve_record(catalog)) {
    return false;
  }
  const std::size_t record = catalog->recordCount;
  record_at(*catalog, record) = metadata;
  reload_generation_at(*catalog, record) = 0U;
  index_insert(catalog->index, catalog->indexCapacity, metadata.assetId,
               record);
  ++catalog->recordCount;
  ++catalog->generation;
  return true;
}

std::uint32_t asset_reload_generation(const AssetCatalog *catalog,
                                      AssetId id) noexcept {
  const std::size_t record = find_record(catalog, id);
  if (record == kNoRecord) {
    return 0U;
  }
  return catalog->pages[record / AssetCatalog::kPageRecords]
      ->reloadGenerations[record % AssetCatalog::kPageRecords];
}

bool note_asset_reloaded(AssetCatalog *catalog, AssetId id) noexcept {
  ENGINE_ASSERT_MAIN_THREAD();
  const std::size_t record = find_record(catalog, id);
  if (record == kNoRecord) {
    return false;
  }
  ++reload_generation_at(*catalog, record);
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
  if ((catalog == nullptr) || (id == kInvalidAssetId)) {
    return false;
  }
  if (find_record(catalog, id) != kNoRecord) {
    return true;
  }
  const std::size_t limit = (catalog->recordLimit < AssetCatalog::kMaxRecords)
                                ? catalog->recordLimit
                                : AssetCatalog::kMaxRecords;
  return catalog->recordCount < limit;
}

const AssetMetadata *find_asset_metadata(const AssetCatalog *catalog,
                                         AssetId id) noexcept {
  const std::size_t record = find_record(catalog, id);
  return (record != kNoRecord) ? &record_at(*catalog, record) : nullptr;
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
  for (std::size_t i = 0U; i < catalog->recordCount; ++i) {
    if (record_at(*catalog, i).ref == ref) {
      return &record_at(*catalog, i);
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
  for (std::size_t i = 0U; i < catalog->recordCount; ++i) {
    const AssetMetadata &record = record_at(*catalog, i);
    if (!asset_ref_is_valid(record.ref)) {
      continue;
    }
    // Only a full ref collision is a duplicate: two outputs of one
    // source share its GUID by design and are told apart by local id.
    bool collides = false;
    for (std::size_t other = 0U; other < catalog->recordCount; ++other) {
      if ((other != i) && (record_at(*catalog, other).ref == record.ref)) {
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
  const std::size_t found = find_record(catalog, id);
  if (found == kNoRecord) {
    return false;
  }

  ENGINE_ASSERT_MAIN_THREAD();
  AssetMetadata &record = record_at(*catalog, found);
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
  const AssetMetadata *record = find_asset_metadata(catalog, id);
  return (record != nullptr) && asset_metadata_has_tag(record, tag);
}

std::size_t query_assets_by_tag(const AssetCatalog *catalog, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept {
  if ((catalog == nullptr) || (tag == nullptr) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; (i < catalog->recordCount) && (count < maxIds);
       ++i) {
    const AssetMetadata &record = record_at(*catalog, i);
    if (asset_metadata_has_tag(&record, tag)) {
      outIds[count++] = record.assetId;
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
  for (std::size_t i = 0U; (i < catalog->recordCount) && (count < maxIds);
       ++i) {
    const AssetMetadata &record = record_at(*catalog, i);
    if (record.typeTag == typeTag) {
      outIds[count++] = record.assetId;
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
  const std::size_t found = find_record(catalog, id);
  if (found == kNoRecord) {
    return false;
  }

  ENGINE_ASSERT_MAIN_THREAD();
  AssetMetadata &record = record_at(*catalog, found);
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

/// Assets already visited during one traversal. A catalogued asset is
/// marked by its record number in a bitset sized to the records the
/// catalog held when the traversal began; an id with no record then, which
/// includes one a load callback registers mid-traversal, is held in a side
/// list instead.
struct VisitedAssets final {
  static constexpr std::size_t kMaxUnregistered = 256U;
  static constexpr std::size_t kMarkBits = 64U;

  std::unique_ptr<std::uint64_t[]> recordMarks{};
  std::size_t markedRecords = 0U;
  AssetId unregistered[kMaxUnregistered] = {};
  std::size_t unregisteredCount = 0U;

  /// Sizes the bitset for `records` records; false when it cannot be
  /// allocated.
  bool reserve(std::size_t records) noexcept {
    const std::size_t words = (records + kMarkBits - 1U) / kMarkBits;
    recordMarks.reset(new (std::nothrow)
                          std::uint64_t[(words > 0U) ? words : 1U]());
    markedRecords = records;
    return recordMarks != nullptr;
  }
};

/// Whether the asset was already visited. `record` is the id's record
/// number, or kNoRecord when the catalog holds none. The side list is
/// scanned either way, because a callback that registers metadata can give
/// an id a record it did not have when it was first visited.
bool asset_visited(const VisitedAssets &visited, std::size_t record,
                   AssetId id) noexcept {
  if (record < visited.markedRecords) {
    const std::uint64_t bit = 1ULL << (record % VisitedAssets::kMarkBits);
    if ((visited.recordMarks[record / VisitedAssets::kMarkBits] & bit) !=
        0ULL) {
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

/// Records that the asset was visited. False only when an id outside the
/// bitset finds the side list full: the caller reports that instead of
/// continuing, because continuing would load an id shared by several
/// dependents once per dependent.
bool mark_asset_visited(VisitedAssets &visited, std::size_t record,
                        AssetId id) noexcept {
  if (record < visited.markedRecords) {
    const std::uint64_t bit = 1ULL << (record % VisitedAssets::kMarkBits);
    visited.recordMarks[record / VisitedAssets::kMarkBits] |= bit;
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

  const std::size_t record = find_record(traversal.catalog, id);
  if (asset_visited(traversal.visited, record, id)) {
    return true;
  }

  traversal.visitStack[visitDepth] = id;

  if (record != kNoRecord) {
    // Copied: a load callback may register records, and the dependency
    // list is read across those calls.
    const AssetMetadata meta = record_at(*traversal.catalog, record);
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

  if (!mark_asset_visited(traversal.visited, record, id)) {
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

/// Whether record `record` lists `id` among its dependencies.
bool record_depends_on(const AssetCatalog &catalog, std::size_t record,
                       AssetId id) noexcept {
  const AssetMetadata &meta = record_at(catalog, record);
  for (std::size_t i = 0U; i < meta.dependencyCount; ++i) {
    if (meta.dependencies[i] == id) {
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
  for (std::size_t i = 0U; i < catalog->recordCount; ++i) {
    if (!record_depends_on(*catalog, i, id)) {
      continue;
    }
    if ((outIds != nullptr) && (count < maxIds)) {
      outIds[count] = record_at(*catalog, i).assetId;
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

  // Every dependent owns a record, so a record bitset remembers what was
  // visited, and a queue of record numbers can never hold more than the
  // catalog. The changed asset starts the walk by id: it may own no record.
  // The visitor may not register records, so the count holds throughout.
  const std::size_t records = catalog->recordCount;
  VisitedAssets visited{};
  std::unique_ptr<std::uint32_t[]> queue(
      new (std::nothrow) std::uint32_t[(records > 0U) ? records : 1U]);
  if (!visited.reserve(records) || (queue == nullptr)) {
    core::log_message(core::LogLevel::Error, "assets",
                      "asset change notification could not allocate its "
                      "walk; dependents were not told");
    return 0U;
  }
  std::size_t head = 0U;
  std::size_t tail = 0U;
  const std::size_t changedRecord = find_record(catalog, changed);
  if (changedRecord != kNoRecord) {
    static_cast<void>(mark_asset_visited(visited, changedRecord, changed));
  }

  AssetId cause = changed;
  for (;;) {
    for (std::size_t i = 0U; i < records; ++i) {
      const AssetId dependent = record_at(*catalog, i).assetId;
      if (asset_visited(visited, i, dependent) ||
          !record_depends_on(*catalog, i, cause)) {
        continue;
      }
      static_cast<void>(mark_asset_visited(visited, i, dependent));
      queue[tail++] = static_cast<std::uint32_t>(i);
      visit(dependent, cause, userData);
    }
    if (head == tail) {
      break;
    }
    cause = record_at(*catalog, queue[head++]).assetId;
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
  // recursion by reference; its visited bitset is sized to the catalog.
  DependencyTraversal traversal{};
  traversal.catalog = catalog;
  traversal.loadCallback = loadCallback;
  traversal.userData = userData;
  if (!traversal.visited.reserve(catalog->recordCount)) {
    core::log_message(core::LogLevel::Error, "assets",
                      "dependency load could not allocate its walk");
    return false;
  }

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
