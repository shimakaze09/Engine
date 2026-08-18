// Compatibility shim: asset identity/metadata moved to engine::content
// (#171 C1). Deletion condition: the C4 consumer migration removes the
// last include of this renderer-path header.

#pragma once

#include "engine/content/asset_metadata.h"

namespace engine::renderer {

using content::AssetId;
using content::kInvalidAssetId;
using content::AssetTypeTag;
using content::MeshImportSettings;
using content::TextureImportSettings;
using content::AssetMetadata;
using content::asset_metadata_has_tag;
using content::asset_metadata_add_tag;
using content::write_metadata_path;
using content::asset_metadata_add_dependency;
using content::asset_metadata_has_dependency;

} // namespace engine::renderer
