// Implements texture hot reload declared in texture_hot_reload.h.

#include "engine/renderer/texture_hot_reload.h"

#include <cstdio>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/material_inheritance.h"

namespace engine::renderer {

namespace {

constexpr const char *kTextureReloadLogChannel = "material";

/// The record for `id` when it is Ready or Failed; nullptr otherwise.
TextureAssetRecord *find_reloadable_record(AssetDatabase *database,
                                           content::AssetId id) noexcept {
  const std::uint32_t *slot = database->textureIndex.find(id);
  if ((slot == nullptr) || !database->textureOccupied[*slot]) {
    return nullptr;
  }
  TextureAssetRecord &record = database->textureAssets[*slot];
  const bool reloadable = (record.state == content::AssetState::Ready) ||
                          (record.state == content::AssetState::Failed);
  return reloadable ? &record : nullptr;
}

/// The path the texture loads from: the catalog's, else the one recorded
/// when it was first loaded.
const char *texture_source_path(const content::AssetCatalog *catalog,
                                const TextureAssetRecord &record) noexcept {
  const content::AssetMetadata *metadata =
      content::find_asset_metadata(catalog, record.id);
  if ((metadata != nullptr) && (metadata->filePath[0] != '\0')) {
    return metadata->filePath.data();
  }
  return (record.sourcePath[0] != '\0') ? record.sourcePath.data() : nullptr;
}

} // namespace

TextureReload reload_texture_asset(AssetDatabase *database,
                                   content::AssetCatalog *catalog,
                                   content::AssetId id,
                                   MaterialTextureLoadFn loadFn,
                                   MaterialTextureReleaseFn releaseFn,
                                   void *userData) noexcept {
  if ((database == nullptr) || (catalog == nullptr) || (loadFn == nullptr)) {
    return TextureReload::NotLoaded;
  }
  TextureAssetRecord *record = find_reloadable_record(database, id);
  if (record == nullptr) {
    return TextureReload::NotLoaded;
  }
  const char *path = texture_source_path(catalog, *record);
  if (path == nullptr) {
    return TextureReload::NotLoaded;
  }

  // Read before the load, as the first load does, so a save that lands
  // during it is picked up by the next poll.
  const std::int64_t writeTime = core::vfs_file_mtime(path);
  const TextureHandle loaded = loadFn(path, userData);
  record->sourceWriteTime = writeTime;
  if (loaded == kInvalidTextureHandle) {
    char message[512] = {};
    std::snprintf(message, sizeof(message), "texture reload failed; %s: %.400s",
                  (record->state == content::AssetState::Ready)
                      ? "the previous texture stays in use"
                      : "materials keep their scalar parameters",
                  path);
    core::log_message(core::LogLevel::Error, kTextureReloadLogChannel, message);
    return TextureReload::Failed;
  }

  const TextureHandle previous = record->runtimeTexture;
  record->runtimeTexture = loaded;
  record->state = content::AssetState::Ready;
  record->requestedResident = true;
  record->refCount = (record->refCount == 0U) ? 1U : record->refCount;
  static_cast<void>(content::note_asset_reloaded(catalog, id));
  static_cast<void>(propagate_material_to_dependents(database, catalog, id));
  if ((previous != kInvalidTextureHandle) && (previous != loaded) &&
      (releaseFn != nullptr)) {
    releaseFn(previous, userData);
  }
  return TextureReload::Reloaded;
}

std::size_t poll_texture_changes(AssetDatabase *database,
                                 content::AssetCatalog *catalog,
                                 MaterialTextureLoadFn loadFn,
                                 MaterialTextureReleaseFn releaseFn,
                                 void *userData) noexcept {
  if ((database == nullptr) || (catalog == nullptr)) {
    return 0U;
  }
  constexpr std::size_t kSlots = AssetDatabase::kMaxTextureAssets;
  std::size_t reloaded = 0U;
  std::size_t slot = database->textureReloadCursor % kSlots;
  for (std::size_t checked = 0U; checked < kTextureReloadPollSlots;
       ++checked, slot = (slot + 1U) % kSlots) {
    if (!database->textureOccupied[slot]) {
      continue;
    }
    const TextureAssetRecord &record = database->textureAssets[slot];
    if ((record.state != content::AssetState::Ready) &&
        (record.state != content::AssetState::Failed)) {
      continue;
    }
    const char *path = texture_source_path(catalog, record);
    if ((path == nullptr) ||
        (core::vfs_file_mtime(path) == record.sourceWriteTime)) {
      continue;
    }
    if (reload_texture_asset(database, catalog, record.id, loadFn, releaseFn,
                             userData) == TextureReload::Reloaded) {
      ++reloaded;
    }
  }
  database->textureReloadCursor = static_cast<std::uint32_t>(slot);
  return reloaded;
}

} // namespace engine::renderer
