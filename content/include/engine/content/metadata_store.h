// Declares the generic asset metadata store (#171 C2): the fixed-slot
// identity/tag/dependency table split out of the renderer's AssetDatabase,
// usable by any asset class with no renderer dependency.

#pragma once

#include <array>
#include <cstddef>

#include "engine/content/asset_metadata.h"

namespace engine::content {

/// Fixed-slot open-addressed metadata table keyed by AssetId.
struct MetadataStore final {
  static constexpr std::size_t kMaxMetadata = 4096U;
  std::array<AssetMetadata, kMaxMetadata> entries{};
  std::array<bool, kMaxMetadata> occupied{};
};

/// Resets every slot back to the empty state.
void clear_metadata_store(MetadataStore *store) noexcept;

/// Inserts or updates the metadata record keyed by its assetId; false when
/// the id is invalid or the table is full.
bool register_asset_metadata(MetadataStore *store,
                             const AssetMetadata &metadata) noexcept;

/// Finds the matching record for the id; nullptr when absent.
const AssetMetadata *find_asset_metadata(const MetadataStore *store,
                                         AssetId id) noexcept;

/// Adds a tag to the id's metadata; false when unknown or tags full.
bool add_asset_tag(MetadataStore *store, AssetId id,
                   const char *tag) noexcept;

/// True when the id's metadata carries the tag.
bool asset_has_tag(const MetadataStore *store, AssetId id,
                   const char *tag) noexcept;

/// Collects up to maxIds ids carrying the tag; returns the count.
std::size_t query_assets_by_tag(const MetadataStore *store, const char *tag,
                                AssetId *outIds, std::size_t maxIds) noexcept;

/// Collects up to maxIds ids of the given type; returns the count.
std::size_t query_assets_by_type(const MetadataStore *store,
                                 AssetTypeTag typeTag, AssetId *outIds,
                                 std::size_t maxIds) noexcept;

/// Copies up to maxIds direct dependencies of the id; returns the count.
std::size_t get_dependencies(const MetadataStore *store, AssetId id,
                             AssetId *outIds, std::size_t maxIds) noexcept;

/// Records a directed dependency edge id -> depId; false when full.
bool add_asset_dependency(MetadataStore *store, AssetId id,
                          AssetId depId) noexcept;

/// Loads an asset and all its dependencies depth-first, dependency-first,
/// invoking loadCallback exactly once per distinct asset in dependency
/// order (callers reach their own state through userData), so a shared
/// dependency is never loaded twice however wide the graph. False on a
/// cycle, excessive depth, a failed callback, or a graph that reaches more
/// ids the store holds no metadata for than the traversal can remember —
/// that limit is reported rather than met by repeating a load.
bool load_with_dependencies(MetadataStore *store, AssetId rootId,
                            bool (*loadCallback)(AssetId id, void *userData),
                            void *userData) noexcept;

} // namespace engine::content
