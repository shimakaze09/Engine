// Gives material tests the asset catalog a material document resolves
// through. A fixture asset is catalogued the way the mount walk would
// catalogue it, with a GUID of its own, and the fixture text then names
// it by that GUID exactly as an authored file does.

#pragma once

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_identity.h"
#include "engine/renderer/asset_database.h"

namespace engine::tests {

/// The text a material document writes for one catalogued asset.
struct MaterialRefText final {
  char text[content::kAssetRefTextLength + 1U] = {};
};

/// Catalogues `virtualPath` as an asset of `type` and returns its
/// reference text; empty text when the catalog refuses the record. The
/// GUID derives from `guidSeed`, the path unless given, so a test can
/// move an asset to a new path and keep its identity, as a rename in the
/// content browser does.
inline MaterialRefText
catalog_material_asset(content::AssetCatalog *catalog, const char *virtualPath,
                       content::AssetTypeTag type,
                       const char *guidSeed = nullptr) noexcept {
  MaterialRefText out{};
  content::AssetMetadata metadata{};
  metadata.assetId = content::make_asset_id_from_path(virtualPath);
  metadata.ref = core::asset_ref_primary(content::builtin_asset_guid(
      (guidSeed != nullptr) ? guidSeed : virtualPath));
  metadata.typeTag = type;
  content::write_metadata_path(&metadata.filePath, virtualPath);
  if (!content::register_asset_metadata(catalog, metadata) ||
      !content::format_asset_ref(metadata.ref, out.text, sizeof(out.text))) {
    out.text[0] = '\0';
  }
  return out;
}

/// Catalogues a texture fixture; see catalog_material_asset.
inline MaterialRefText catalog_texture(content::AssetCatalog *catalog,
                                       const char *virtualPath) noexcept {
  return catalog_material_asset(catalog, virtualPath,
                                content::AssetTypeTag::Texture);
}

/// Catalogues a material fixture; see catalog_material_asset.
inline MaterialRefText catalog_material(content::AssetCatalog *catalog,
                                        const char *virtualPath) noexcept {
  return catalog_material_asset(catalog, virtualPath,
                                content::AssetTypeTag::Material);
}

} // namespace engine::tests
