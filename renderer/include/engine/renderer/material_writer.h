// Declares JSON material asset saving for the Engine renderer system.
//
// Serializes a resolved Material plus its texture-slot references into the
// schema documented in material_loader.h and commits the result with staged
// atomic replacement (core::atomic_write_file): a write, flush, or rename
// failure leaves the previous file on disk completely untouched.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/material.h"

namespace engine::renderer {

/// Finds the material's parent, if any: the one dependency (recorded by the
/// loader) whose own metadata is Material-tagged rather than Texture-tagged.
/// Returns false when the material has no parent or its id/metadata cannot
/// be resolved; outPath is left untouched in that case.
bool find_material_parent_virtual_path(const AssetDatabase *database,
                                       AssetId materialId, char *outPath,
                                       std::size_t outPathCapacity) noexcept;

/// Serializes `params`/`textureSlots` as a material document and writes it
/// to the OS path behind `virtualPath` with staged atomic replacement.
/// `parentVirtualPath` may be null/empty for no parent. With no parent every
/// field is written; with one, only the fields `overriddenFields`
/// (material_field bits, see material_overrides) names, so the saved
/// material stays an instance its parent's edits still reach. A written
/// texture slot whose AssetId is set but whose source path cannot be
/// resolved through the database's metadata fails the save outright
/// (logged) rather than silently dropping the reference. False on any
/// failure; the destination file is guaranteed untouched
/// (atomic_write_file's contract) whenever this returns false.
bool save_material_asset(const AssetDatabase *database, const char *virtualPath,
                         const Material &params,
                         const MaterialTextureSlots &textureSlots,
                         const char *parentVirtualPath,
                         std::uint16_t overriddenFields) noexcept;

} // namespace engine::renderer
