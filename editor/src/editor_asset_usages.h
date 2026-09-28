// Declares Find Usages for the Assets panel: which authored documents
// (scenes, prefabs, materials, animation controllers) reference an asset.
// Documents name most assets by AssetRef — a GUID, plus "#<local id>" for
// a sub-asset such as a mesh cooked from a glTF — and a few by path
// (scripts, controllers, clips), so a document references the asset when
// it holds either as a whole JSON string, as Unity finds references by
// matching an asset's GUID across its text assets.

#pragma once

#include "editor_asset_index.h"

#include "engine/core/asset_identity.h"

#include <cstddef>

namespace engine::editor {

/// True for the asset kinds whose documents can reference other assets.
bool asset_kind_references_assets(content::AssetTypeTag kind) noexcept;

/// True when the JSON `text` holds a string that names the asset: exactly
/// `"<virtualPath>"`, or `"<ref>"` in the AssetRef text form. A primary
/// reference (no local id) also matches its sub-assets, `"<guid>#…"`,
/// since they come from the same file. The GUID compares without regard
/// to letter case, as the parser reads it. An empty path or a nil
/// reference matches nothing by that route.
bool document_references_asset(const char *text, const char *virtualPath,
                               const core::AssetRef &ref) noexcept;

/// Documents found by one search; the first kMaxAssetUsages are kept.
struct AssetUsages final {
  static constexpr std::size_t kMaxAssetUsages = 32U;
  char paths[kMaxAssetUsages][kMaxAssetIndexPath] = {};
  std::size_t count = 0U;
  /// More documents referenced the asset than were kept.
  bool truncated = false;
  /// Documents that could not be searched (unreadable, or over the size a
  /// search reads), each logged as a warning.
  std::size_t skipped = 0U;
};

/// Searches every indexed document that can reference assets, except the
/// target itself, for `target`'s path or `ref`, writing the matches'
/// file paths in index order. Cold path, run on an explicit request: it
/// reads each candidate file into one buffer allocated for the search.
/// False, logged, when that buffer cannot be allocated.
bool find_asset_usages(const AssetIndexEntry &target,
                       const core::AssetRef &ref, AssetUsages *out) noexcept;

/// Runs find_asset_usages for `target` and asks for the Find Usages
/// window to open with the result. Callable from anywhere, a row's
/// context menu included: the window is opened by
/// draw_find_usages_popup, in the scope that draws it, since a popup
/// opened from inside another popup's ID scope is never the one drawn.
void request_find_usages(const AssetIndexEntry &target,
                         const core::AssetRef &ref) noexcept;

/// Opens the Find Usages window when one was requested and draws it: the
/// documents that use the asset, each opened by a double click, and
/// whether the list was cut short or documents were skipped. Call every
/// frame from one fixed place in the Assets panel.
void draw_find_usages_popup() noexcept;

/// The last requested search's result.
const AssetUsages &last_find_usages() noexcept;

} // namespace engine::editor
