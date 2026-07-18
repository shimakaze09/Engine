// Declares JSON material asset loading for the Engine renderer system.
//
// A material asset is a JSON file describing PBR parameters. It may name a
// parent material; the child starts from the parent's resolved values and
// overrides only the fields it specifies (material instancing). Resolution
// is baked at load time so runtime lookups stay flat. Texture references
// are not part of the v1 schema (parameters only).
//
// Schema (all fields optional unless noted):
//   {
//     "version": 1,                       // accepted versions: 1
//     "parent": "assets/materials/x.json",
//     "albedo": [r, g, b],
//     "emissive": [r, g, b],
//     "roughness": 0.5,
//     "metallic": 0.0,
//     "opacity": 1.0
//   }
// Parsing is strict: a present-but-malformed field rejects the load.

#pragma once

#include "engine/renderer/asset_database.h"

namespace engine::renderer {

/// Maximum parent-chain depth; deeper chains (including cycles) fail.
inline constexpr std::size_t kMaxMaterialParentDepth = 8U;

/// Loads a material JSON file (through the core VFS), resolves its parent
/// chain, registers the record plus Material-tagged metadata (with a
/// dependency edge to the parent), and returns its path-derived AssetId in
/// outId. Logs and returns false on IO, parse, or depth failures.
bool load_material_asset(AssetDatabase *database, const char *virtualPath,
                         AssetId *outId) noexcept;

} // namespace engine::renderer
