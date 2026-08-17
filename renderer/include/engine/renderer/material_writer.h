// Declares JSON material asset saving for the Engine renderer system.
//
// Serializes a resolved Material plus its texture-slot references back into
// the v2 schema documented in material_loader.h and commits the result with
// staged atomic replacement (core::atomic_write_file): a write, flush, or
// rename failure leaves the previous file on disk completely untouched.
// Editor saves always emit v2 — this is the schema migration path a v1 file
// takes the moment an author edits it in the material editor; files no
// author has touched keep loading as v1 exactly as before.

#pragma once

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

/// Serializes `params`/`textureSlots` as a v2 material document and writes
/// it to the OS path behind `virtualPath` with staged atomic replacement.
/// `parentVirtualPath` may be null/empty for no parent. A texture slot
/// whose AssetId is set but whose source path cannot be resolved through
/// the database's metadata fails the save outright (logged) rather than
/// silently dropping the reference. False on any failure; the destination
/// file is guaranteed untouched (atomic_write_file's contract) whenever
/// this returns false.
bool save_material_asset(const AssetDatabase *database,
                         const char *virtualPath, const Material &params,
                         const MaterialTextureSlots &textureSlots,
                         const char *parentVirtualPath) noexcept;

} // namespace engine::renderer
