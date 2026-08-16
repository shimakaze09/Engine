// Declares the editor's cached content-browser asset index: a one-time
// filesystem walk with per-entry type classification, change-driven filter
// caching, and folder navigation queries. Keeps editor_panels_assets.cpp
// free of per-frame directory walks or O(assets) string scans (issue #157).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine::editor {

/// Enumerates the browsable asset categories the content browser filters
/// and dispatches typed actions on. Distinct from
/// engine::renderer::AssetTypeTag: that enum tags GPU-resident cooked
/// records already loaded into the runtime AssetDatabase, while this one
/// classifies every source file the editor can see on disk, including
/// kinds (Scene, Animation, AnimationController) the renderer database
/// never tracks.
enum class AssetKind : std::uint8_t {
  Mesh,
  Texture,
  Material,
  Script,
  Scene,
  Animation,
  AnimationController,
  Sound,
  Other,
};

/// Number of AssetKind values (used to size the type-filter bitmask).
inline constexpr std::size_t kAssetKindCount = 9U;
/// Bitmask bit for one AssetKind, used by AssetFilterState::typeMask.
inline constexpr std::uint32_t asset_kind_bit(AssetKind kind) noexcept {
  return 1U << static_cast<std::uint32_t>(kind);
}
/// Type-filter mask value that accepts every AssetKind.
inline constexpr std::uint32_t kAssetKindMaskAll =
    (1U << kAssetKindCount) - 1U;

constexpr std::size_t kMaxAssetIndexPath = 512U;
constexpr std::size_t kMaxAssetIndexName = 160U;

/// One indexed asset: display/search fields plus its classified kind.
/// osPath is the filesystem path used for thumbnails, file reveal, and
/// scene-document open (which validates against the OS asset-root jail);
/// virtualPath is the "<mount>/<relative>" VFS form used by mesh spawn and
/// drag-drop (empty when the entry falls outside the configured mount).
struct AssetIndexEntry final {
  char osPath[kMaxAssetIndexPath] = {};
  char virtualPath[kMaxAssetIndexPath] = {};
  char name[kMaxAssetIndexName] = {};
  char folder[kMaxAssetIndexPath] = {};
  AssetKind kind = AssetKind::Other;
  bool hasThumbnail = false;
};

/// Rebuilds the process-wide asset index by walking editor_asset_root()
/// once. Cold path: called on explicit rescan and editor startup, never
/// per frame. Skips sidecar/internal files (.meta.json, .cookstamp,
/// .checksum) and the .thumbnails cache directories. False when the asset
/// root does not exist (the index is cleared either way).
bool rebuild_asset_index() noexcept;
/// Number of currently indexed entries.
std::size_t asset_index_count() noexcept;
/// Entry at `index`, or nullptr when out of range.
const AssetIndexEntry *asset_index_entry(std::size_t index) noexcept;
/// Monotonic counter bumped by every rebuild_asset_index call so dependent
/// filter caches can detect staleness without a deep compare.
std::uint64_t asset_index_generation() noexcept;
/// True once rebuild_asset_index has run at least once this process.
bool asset_index_built() noexcept;

/// Classifies one file by extension, falling back to a cheap top-level-key
/// content sniff for ambiguous ".json" files (scene/material/animation
/// controller all use that extension). Exposed for tests.
AssetKind classify_asset_kind(const char *osPath) noexcept;
/// Display label for a kind ("Mesh", "Texture", ...).
const char *asset_kind_label(AssetKind kind) noexcept;

/// Search/filter/navigation request evaluated against the index.
struct AssetFilterState final {
  char query[256] = {};
  std::uint32_t typeMask = kAssetKindMaskAll;
  // "" scopes to the index root; folder-view scoping is ignored when
  // flatSearch is set.
  char folder[kMaxAssetIndexPath] = {};
  bool flatSearch = false;

  bool operator==(const AssetFilterState &other) const noexcept;
};

/// Cached filtered result: recomputed only when the requested filter or
/// the index generation differs from what produced the cached matches.
struct AssetFilterCache final {
  std::uint64_t appliedGeneration = 0ULL;
  AssetFilterState appliedFilter{};
  bool valid = false;
  std::vector<std::size_t> matches{};
};

/// Recomputes cache->matches only when `filter` or the index generation
/// changed since the cache's last apply; returns true when a recompute
/// happened (false means the cached matches were already current).
bool refresh_asset_filter_cache(const AssetFilterState &filter,
                                AssetFilterCache *cache) noexcept;

/// True when `entry` matches `filter`: case-insensitive substring match on
/// name/virtualPath for a non-empty query, the type mask, and (unless
/// flatSearch) direct membership in the requested folder.
bool asset_entry_matches_filter(const AssetIndexEntry &entry,
                                const AssetFilterState &filter) noexcept;

/// Cached listing of one folder's distinct immediate child folder OS paths
/// (deduped, sorted); recomputed only when the requested folder or the
/// index generation differs from what produced the cached children —
/// mirrors AssetFilterCache so folder-view draw code never re-walks the
/// index every frame.
struct AssetChildFolderCache final {
  std::uint64_t appliedGeneration = 0ULL;
  char appliedFolder[kMaxAssetIndexPath] = {};
  bool valid = false;
  std::vector<std::string> children{};
};

/// Recomputes cache->children only when `folder` or the index generation
/// changed since the cache's last apply; returns true when a recompute
/// happened.
bool refresh_child_folder_cache(const char *folder,
                                AssetChildFolderCache *cache) noexcept;

/// Enumerates the routed behavior for a double-click/typed Open action.
enum class AssetOpenAction : std::uint8_t {
  SpawnMesh,
  OpenScene,
  SelectOnly,
};

/// Maps an asset kind to its typed Open behavior (pure — the caller
/// performs the actual side effect through the production entry point:
/// execute_asset_spawn for SpawnMesh, request_scene_open for OpenScene).
AssetOpenAction resolve_asset_open_action(AssetKind kind) noexcept;

} // namespace engine::editor
