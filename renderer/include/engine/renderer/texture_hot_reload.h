// Declares texture hot reload: re-reading a material texture whose source
// file changed, swapping it in behind the database record, and telling
// every material that uses it through the catalog's dependency edges. It
// is also how a texture that failed to load recovers once its file is
// fixed: a Failed record is reloaded like a Ready one.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/content/asset_catalog.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

namespace engine::renderer {

/// Releases a texture handle the database no longer serves; paired with
/// the MaterialTextureLoadFn that made it.
using MaterialTextureReleaseFn = void (*)(TextureHandle handle,
                                          void *userData) noexcept;

/// What reload_texture_asset did.
enum class TextureReload : std::uint8_t {
  /// The texture is Ready with the file's current contents, and every
  /// material that uses it was told.
  Reloaded,
  /// The id has no Ready or Failed record: nothing loaded it, so there is
  /// nothing to replace.
  NotLoaded,
  /// The file did not load. A Ready texture keeps serving its previous
  /// handle and a Failed one stays Failed; the attempt is logged once and
  /// its file time recorded, so the next save tries again.
  Failed,
};

/// Loads the texture `id` again from the path the catalog records for it.
/// On success the record takes the new handle and becomes Ready, the
/// previous handle is released through `releaseFn`, the catalog records
/// the reload (content::note_asset_reloaded), and
/// content::notify_asset_changed reaches every material that uses it:
/// one that names it drops that slot's handle, one inheriting it takes the
/// parent's, and the next resolve_material_textures fetches the new one.
TextureReload reload_texture_asset(AssetDatabase *database,
                                   content::AssetCatalog *catalog,
                                   content::AssetId id,
                                   MaterialTextureLoadFn loadFn,
                                   MaterialTextureReleaseFn releaseFn,
                                   void *userData) noexcept;

/// Texture records one poll_texture_changes call checks.
inline constexpr std::size_t kTextureReloadPollSlots = 64U;

/// Checks the next kTextureReloadPollSlots texture records, resuming where
/// the last call stopped, and reloads each Ready or Failed one whose source
/// file time differs from the recorded one. One stat per occupied record
/// checked; a full table is swept every
/// AssetDatabase::kMaxTextureAssets / kTextureReloadPollSlots calls. Editor
/// only: a player's content does not change under it. Returns how many
/// textures reloaded.
std::size_t poll_texture_changes(AssetDatabase *database,
                                 content::AssetCatalog *catalog,
                                 MaterialTextureLoadFn loadFn,
                                 MaterialTextureReleaseFn releaseFn,
                                 void *userData) noexcept;

} // namespace engine::renderer
