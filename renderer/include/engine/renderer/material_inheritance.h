// Declares material parent-chain resolution over the asset database: which
// material a material inherits from, and how a change to one reaches every
// material that inherits from it.
//
// A material record holds fully resolved values, so render prep reads it
// flat. What keeps those values honest is the record's override mask
// (MaterialAssetRecord::overriddenFields): the fields it authors are its
// own, and every other field is re-taken from its parent whenever the
// parent changes, in memory, without re-reading any file.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/material.h"

namespace engine::renderer {

/// The material `materialId` inherits from: its one Material-tagged
/// dependency. kInvalidAssetId when it has none or is unknown.
AssetId find_material_parent_id(const content::AssetCatalog *catalog,
                                AssetId materialId) noexcept;

/// Whether `target` is `from` or one of its ancestors: true when making
/// `from` the parent of `target` would close a cycle.
bool material_chain_contains(const content::AssetCatalog *catalog, AssetId from,
                             AssetId target) noexcept;

/// Re-resolves every loaded material that inherits from `changedId`,
/// directly or through other materials, from its parent's current values
/// and its own overrides. Each material is visited once. Returns how many
/// were re-resolved.
std::size_t
propagate_material_to_dependents(AssetDatabase *database,
                                 const content::AssetCatalog *catalog,
                                 AssetId changedId) noexcept;

/// Applies an editor edit to a loaded material: every field that differs
/// from the record becomes one this material authors, the record takes the
/// new values, and the change reaches the material's dependents. False when
/// the id is not a loaded material.
bool edit_material_asset(AssetDatabase *database,
                         const content::AssetCatalog *catalog,
                         AssetId materialId, const Material &params,
                         const MaterialTextureSlots &textureSlots) noexcept;

/// Sets a loaded material to exactly this state, overrides included -- an
/// editor undo or redo, which must also restore which fields the material
/// authors -- and re-resolves its dependents. False when the id is not a
/// loaded material.
bool restore_material_asset(AssetDatabase *database,
                            const content::AssetCatalog *catalog,
                            AssetId materialId, const Material &params,
                            const MaterialTextureSlots &textureSlots,
                            std::uint16_t overriddenFields) noexcept;

} // namespace engine::renderer
