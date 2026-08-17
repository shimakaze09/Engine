// Declares JSON material asset loading for the Engine renderer system.
//
// A material asset is a JSON file describing PBR parameters and (v2)
// texture-slot references. It may name a parent material; the child starts
// from the parent's resolved values and overrides only the fields it
// specifies (material instancing), including texture references. Scalar/
// vector resolution is baked at load time so runtime lookups stay flat;
// texture *references* are resolved the same way (to path-derived AssetIds,
// inherited down the parent chain like every other field), but the GPU
// TextureHandle behind each reference is filled in later by
// resolve_material_textures, since that requires a live GL context and must
// never run on a per-frame hot path.
//
// Schema v1 (all fields optional unless noted) is preserved exactly:
//   {
//     "version": 1,                       // absent or 1 both mean v1
//     "parent": "assets/materials/x.json",
//     "albedo": [r, g, b],
//     "emissive": [r, g, b],
//     "roughness": 0.5,
//     "metallic": 0.0,
//     "opacity": 1.0
//   }
// A v1 file (version absent or 1) never parses the v2-only fields below,
// even if present in the JSON, so existing content's behavior cannot change
// out from under it (CLAUDE.md staged-migration rule).
//
// Schema v2 adds explicit alpha handling, UV transform, and texture-slot
// references, additive over v1:
//   {
//     "version": 2,
//     ... every v1 field above, plus ...
//     "alphaMode": "opaque" | "mask" | "blend",  // default "opaque"
//     "alphaCutoff": 0.5,                         // default 0.5, Mask only
//     "uvTiling": [1.0, 1.0],
//     "uvOffset": [0.0, 0.0],
//     "textures": {
//       "albedo": "assets/textures/x_albedo.png",
//       "metallicRoughness": "assets/textures/x_mr.png", // glTF-style: G
//                                                          // = roughness,
//                                                          // B = metallic
//       "emissive": "assets/textures/x_emissive.png",
//       "occlusion": "assets/textures/x_ao.png",   // R channel
//       "opacity": "assets/textures/x_opacity.png" // R channel, cutout mask
//     }
//   }
// Normal-map textures are intentionally not part of the schema: the vertex
// format carries no tangent basis yet, so a normal-map slot would be a
// texture reference no shader pass ever samples (explicitly cut by
// issue #160's "no partially implemented slot" rule) until a follow-up adds
// tangent-space vertex data.
// Parsing is strict: a present-but-malformed field rejects the load.

#pragma once

#include <expected>

#include "engine/renderer/asset_database.h"
#include "engine/renderer/texture_loader.h"

namespace engine::renderer {

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
std::expected<AssetId, MaterialLoadError>
load_material_asset(AssetDatabase *database, const char *virtualPath) noexcept;

/// Re-reads and re-parses an already-loaded material file in place (editor
/// save/hot-reload). On success the material's params/textureSlots/metadata
/// are fully replaced (texture GPU handles are cleared back to unresolved so
/// resolve_material_textures re-fetches them next sync). On any failure
/// (missing file, malformed JSON, bad parent, full tables) the existing
/// record is left completely untouched and the previous valid state keeps
/// serving renders — never a partial or corrupt in-place update.
std::expected<AssetId, MaterialLoadError>
reload_material_asset(AssetDatabase *database,
                      const char *virtualPath) noexcept;

/// Loads every *.json under an OS directory as material assets addressed as
/// "<virtualPrefix>/<filename>" (sorted, so registration order is
/// deterministic). Returns the number successfully loaded; boot-time only.
std::size_t load_material_assets_in_directory(
    AssetDatabase *database, const char *osDirectory,
    const char *virtualPrefix) noexcept;

/// Loads one texture from a VFS virtual path and returns its handle
/// (kInvalidTextureHandle on failure); the production texture-loader
/// callback resolve_material_textures is driven with. A GL context must be
/// current.
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
/// parameters — never a crash, never a stale/unrelated texture bind. Not a
/// per-frame hot path itself, but cheap to call every frame: cost is O(materials
/// with an unresolved slot), which drains to zero once textures are
/// resident. Returns the number of texture slots newly resolved to Ready.
std::size_t resolve_material_textures(AssetDatabase *database,
                                      MaterialTextureLoadFn loadFn,
                                      void *userData) noexcept;

} // namespace engine::renderer
