// Declares JSON material asset loading for the Engine renderer system.
//
// A material asset is a ".mat" JSON document of PBR parameters, a shading
// model and texture-slot references. It may name a parent material: every
// field the document names is the material's own, and every field it omits
// is its parent's. That split is kept on the record
// (MaterialAssetRecord::overriddenFields), so when a parent is edited or
// reloaded its dependents are re-resolved in memory from the parent's new
// values and their own overrides (material_inheritance.h); render prep
// still reads one flat, fully resolved record. Texture *references*
// inherit the same way; the GPU TextureHandle behind each is filled in
// later by resolve_material_textures, since that needs a live render
// device and never runs on a per-frame hot path.
//
// The parent and every texture are named by persistent identity, the
// AssetRef text a scene uses (content/asset_ref_json.h), never by path:
// moving or renaming the file keeps its sidecar GUID, so the material
// still finds it. Each reference resolves through the asset catalog at
// load. A reference the catalog has no asset for, or one naming an asset
// of the wrong type, refuses the load rather than dropping the slot, since
// a material loaded without it would save back without it.
//
// Exactly one revision loads, kMaterialDocumentVersion. A document naming
// another version, or none, is refused, not migrated:
//   {
//     "version": 4,                                // required
//     "parent": "<guid>",                          // optional
//     "shadingModel": "pbr" | "toon" | "unlit",    // default "pbr"
//     "albedo": [r, g, b],
//     "emissive": [r, g, b],
//     "roughness": 0.5,
//     "metallic": 0.0,
//     "opacity": 1.0,
//     "alphaMode": "opaque" | "mask" | "blend",    // default "opaque"
//     "alphaCutoff": 0.5,                          // Mask only
//     "uvTiling": [1.0, 1.0],
//     "uvOffset": [0.0, 0.0],
//     "textures": {                                // each "<guid>" or null
//       "albedo": "<guid>",
//       "metallicRoughness": "<guid>",  // glTF-style: G = roughness,
//                                       // B = metallic
//       "emissive": "<guid>",
//       "occlusion": "<guid>",          // R channel
//       "opacity": "<guid>"             // R channel, cutout mask
//     }
//   }
// A texture slot written as null clears a slot the parent fills; a
// material with no parent has nothing to clear and refuses null.
// Every field but "version" is optional; the authoritative list of fields
// is ENGINE_MATERIAL_PARAM_FIELDS / ENGINE_MATERIAL_TEXTURE_FIELDS in
// asset_database.h. A parent chain deeper than kMaxMaterialParentDepth, or
// one that would close a cycle, is refused.
// Normal-map textures are intentionally not part of the schema: the vertex
// format carries no tangent basis, so a normal-map slot would be a texture
// reference no shader pass ever samples. A slot is added only once a pass
// consumes it, which needs tangent-space vertex data first.
// Parsing is strict: a present-but-malformed field rejects the load.

#pragma once

#include <cstdint>
#include <expected>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

/// The one material document revision this build reads and writes. An
/// older revision is refused rather than migrated: the project is
/// unreleased, so the tree migrates once per format change instead of
/// carrying a read path per past revision. A reader that guessed would
/// drop the fields it no longer knows and resave the material as a
/// reduction of itself.
inline constexpr std::uint32_t kMaterialDocumentVersion = 4U;

/// Maximum parent-chain depth; deeper chains (including cycles) fail.
inline constexpr std::size_t kMaxMaterialParentDepth = 8U;

/// Failure classes for material loading; details are already logged when a
/// class is returned. Parent-chain failures surface as Parse.
enum class MaterialLoadError : std::uint8_t {
  InvalidArgument,
  Io,
  Parse,
};

/// Loads a material JSON file (through the core VFS), resolves its parent
/// chain, registers the record plus Material-tagged metadata (with
/// dependency edges to the parent and every referenced texture), and
/// returns its path-derived AssetId. Never call .value(): with exceptions
/// disabled it aborts — check has_value() and use operator* / error().
std::expected<content::AssetId, MaterialLoadError>
load_material_asset(AssetDatabase *database, content::AssetCatalog *catalog,
                    const char *virtualPath) noexcept;

/// Re-reads and re-parses an already-loaded material file in place (editor
/// save/hot-reload). On success the material's params/textureSlots/metadata
/// are fully replaced (texture GPU handles are cleared back to unresolved so
/// resolve_material_textures re-fetches them next sync), and every material
/// inheriting from it is re-resolved. On any failure (missing file,
/// malformed JSON, bad parent, a parent cycle, full tables) the existing
/// record is left completely untouched and the previous valid state keeps
/// serving renders — never a partial or corrupt in-place update.
std::expected<content::AssetId, MaterialLoadError>
reload_material_asset(AssetDatabase *database, content::AssetCatalog *catalog,
                      const char *virtualPath) noexcept;

/// Loads every *.mat under an OS directory as material assets addressed as
/// "<virtualPrefix>/<filename>" (sorted, so registration order is
/// deterministic). Returns the number successfully loaded; boot-time only.
std::size_t load_material_assets_in_directory(
    AssetDatabase *database, content::AssetCatalog *catalog,
    const char *osDirectory, const char *virtualPrefix) noexcept;

/// Loads one texture from a VFS virtual path and returns its handle
/// (kInvalidTextureHandle on failure); the production texture-loader
/// callback resolve_material_textures is driven with. A live render device
/// is required.
using MaterialTextureLoadFn = TextureHandle (*)(const char *virtualPath,
                                                 void *userData) noexcept;

/// Resolves every material's authored texture-slot references (see
/// MaterialTextureSlots) into GPU handles on the material's flat Material
/// record, so render prep only ever reads already-resolved handles. Calls
/// loadFn once per not-yet-attempted texture asset id (Unloaded state);
/// already-Ready or already-Failed ids are looked up instead of reloaded,
/// so a texture shared by several materials is only uploaded once and a
/// failing texture is not retried every call. A failed load is recorded as
/// AssetState::Failed and logged once (actionable: source path + material
/// path); the material's corresponding TextureHandle field simply stays
/// kInvalidTextureHandle, so shaders fall back to the material's scalar
/// parameters — never a crash, never a stale/unrelated texture bind. A
/// texture the full texture table has no room to record is not loaded at
/// all: it is reported once and the slot is not tried again until the
/// material's slots are next assigned. A Failed texture is loaded again
/// only by the editor's hot-reload poll, once its file changes
/// (texture_hot_reload.h). Called every frame: each call visits every
/// loaded material's slots, one table lookup per set slot, and loads
/// nothing once every referenced texture is Ready or Failed. Returns the
/// number of texture slots newly resolved to Ready.
std::size_t resolve_material_textures(AssetDatabase *database,
                                      const content::AssetCatalog *catalog,
                                      MaterialTextureLoadFn loadFn,
                                      void *userData) noexcept;

} // namespace engine::renderer
