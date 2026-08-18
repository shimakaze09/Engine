// Implements the generic asset metadata store and asset-id constructors
// for the Engine content system (#171 C2: split out of the renderer's
// AssetDatabase; the table and its tag/dependency/query logic moved
// verbatim, renderer-free).

#include "engine/content/metadata_store.h"

#include <cstdio>

#include "engine/core/hash.h"
#include "engine/core/logging.h"

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
void clear_metadata_store(MetadataStore *store) noexcept {
  if (store == nullptr) {
    return;
  }

  for (std::size_t i = 0U; i < store->entries.size(); ++i) {
    store->occupied[i] = false;
    store->entries[i] = AssetMetadata{};
  }
}

// --- Metadata management ---

namespace {

/// Finds the matching object or resource for metadata slot.
std::size_t find_metadata_slot(const MetadataStore *store,
                               AssetId id) noexcept {
  if ((store == nullptr) || (id == kInvalidAssetId)) {
    return store != nullptr ? store->entries.size() : 0U;
  }

  const std::size_t capacity = store->entries.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!store->occupied[slot]) {
      return capacity;
    }
    if (store->entries[slot].assetId == id) {
      return slot;
    }
  }

  return capacity;
}

/// Finds the matching object or resource for metadata insert slot.
std::size_t find_metadata_insert_slot(const MetadataStore *store,
                                      AssetId id) noexcept {
  if (store == nullptr) {
    return 0U;
  }

  const std::size_t capacity = store->entries.size();
  const std::size_t base = hashed_slot(id, capacity);
  for (std::size_t probe = 0U; probe < capacity; ++probe) {
    const std::size_t slot = (base + probe) % capacity;
    if (!store->occupied[slot] ||
        (store->entries[slot].assetId == id)) {
      return slot;
    }
  }

  return capacity;
}

} // namespace

bool register_asset_metadata(MetadataStore *store,
                             const AssetMetadata &metadata) noexcept {
  if ((store == nullptr) || (metadata.assetId == kInvalidAssetId) ||
      (metadata.tagCount > AssetMetadata::kMaxTags) ||
      (metadata.dependencyCount > AssetMetadata::kMaxDependencies)) {
    return false;
  }

  const std::size_t slot =
      find_metadata_insert_slot(store, metadata.assetId);
  if (slot == store->entries.size()) {
    return false;
  }

  store->occupied[slot] = true;
  store->entries[slot] = metadata;
  return true;
}

/// Finds the matching object or resource for asset metadata.
const AssetMetadata *find_asset_metadata(const MetadataStore *store,
                                         AssetId id) noexcept {
  if ((store == nullptr) || (id == kInvalidAssetId)) {
    return nullptr;
  }

  const std::size_t slot = find_metadata_slot(store, id);
  if (slot == store->entries.size()) {
    return nullptr;
  }

  return &store->entries[slot];
}

bool add_asset_tag(MetadataStore *store, AssetId id,
                   const char *tag) noexcept {
  if ((store == nullptr) || (id == kInvalidAssetId) || (tag == nullptr)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(store, id);
  if (slot == store->entries.size()) {
    return false;
  }

  return asset_metadata_add_tag(&store->entries[slot], tag);
}

bool asset_has_tag(const MetadataStore *store, AssetId id,
                   const char *tag) noexcept {
  if ((store == nullptr) || (id == kInvalidAssetId) || (tag == nullptr)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(store, id);
  if (slot == store->entries.size()) {
    return false;
  }

  return asset_metadata_has_tag(&store->entries[slot], tag);
}

std::size_t query_assets_by_tag(const MetadataStore *store, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept {
  if ((store == nullptr) || (tag == nullptr) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; i < store->entries.size(); ++i) {
    if (!store->occupied[i]) {
      continue;
    }
    if (asset_metadata_has_tag(&store->entries[i], tag)) {
      outIds[count] = store->entries[i].assetId;
      ++count;
      if (count >= maxIds) {
        break;
      }
    }
  }
  return count;
}

std::size_t query_assets_by_type(const MetadataStore *store,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept {
  if ((store == nullptr) || (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  std::size_t count = 0U;
  for (std::size_t i = 0U; i < store->entries.size(); ++i) {
    if (!store->occupied[i]) {
      continue;
    }
    if (store->entries[i].typeTag == typeTag) {
      outIds[count] = store->entries[i].assetId;
      ++count;
      if (count >= maxIds) {
        break;
      }
    }
  }
  return count;
}

// --- Dependency management ---

std::size_t get_dependencies(const MetadataStore *store, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept {
  if ((store == nullptr) || (id == kInvalidAssetId) || (outIds == nullptr) ||
      (maxIds == 0U)) {
    return 0U;
  }

  const AssetMetadata *meta = find_asset_metadata(store, id);
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

bool add_asset_dependency(MetadataStore *store, AssetId id,
                          AssetId depId) noexcept {
  if ((store == nullptr) || (id == kInvalidAssetId) ||
      (depId == kInvalidAssetId)) {
    return false;
  }

  const std::size_t slot = find_metadata_slot(store, id);
  if (slot == store->entries.size()) {
    return false;
  }

  return asset_metadata_add_dependency(&store->entries[slot], depId);
}

namespace {

/// Depth-first dependency load: rejects cycles via the visit stack,
/// skips assets already loaded this session, and loads every dependency
/// before the asset itself.
bool load_with_deps_recursive(MetadataStore *store, AssetId id,
                              bool (*loadCallback)(AssetId id, void *userData),
                              void *userData, AssetId *visitStack,
                              std::size_t visitDepth, std::size_t maxVisitDepth,
                              AssetId *loadedSet, std::size_t *loadedCount,
                              std::size_t maxLoaded) noexcept {
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

  for (std::size_t i = 0U; i < *loadedCount; ++i) {
    if (loadedSet[i] == id) {
      return true;
    }
  }

  visitStack[visitDepth] = id;

  const AssetMetadata *meta = find_asset_metadata(store, id);
  if (meta != nullptr) {
    for (std::size_t i = 0U; i < meta->dependencyCount; ++i) {
      const AssetId depId = meta->dependencies[i];
      if (depId == kInvalidAssetId) {
        continue;
      }

      if (!load_with_deps_recursive(store, depId, loadCallback, userData,
                                    visitStack, visitDepth + 1U, maxVisitDepth,
                                    loadedSet, loadedCount, maxLoaded)) {
        return false;
      }
    }
  }

  if (loadCallback != nullptr) {
    if (!loadCallback(id, userData)) {
      return false;
    }
  }

  if (*loadedCount < maxLoaded) {
    loadedSet[*loadedCount] = id;
    ++(*loadedCount);
  }

  return true;
}

} // namespace

/// Loads the requested resource for with dependencies.
bool load_with_dependencies(MetadataStore *store, AssetId rootId,
                            bool (*loadCallback)(AssetId id, void *userData),
                            void *userData) noexcept {
  if ((store == nullptr) || (rootId == kInvalidAssetId)) {
    return false;
  }

  constexpr std::size_t kMaxDepth = 64U;
  constexpr std::size_t kMaxLoaded = 256U;
  AssetId visitStack[kMaxDepth] = {};
  AssetId loadedSet[kMaxLoaded] = {};
  std::size_t loadedCount = 0U;

  return load_with_deps_recursive(store, rootId, loadCallback, userData,
                                  visitStack, 0U, kMaxDepth, loadedSet,
                                  &loadedCount, kMaxLoaded);
}

// --- Asset identity constructors (#172: the one shared id hash) ---

/// FNV-1a over the path with separators canonicalized to '/' so the same
/// asset hashes identically on every platform.
AssetId make_asset_id_from_path(const char *path) noexcept {
  if (path == nullptr) {
    return kInvalidAssetId;
  }

  std::uint64_t hash = core::kFnv1a64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(path);
       *cursor != 0U; ++cursor) {
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
