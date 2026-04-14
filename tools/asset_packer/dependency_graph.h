#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine::tools {

/// Build-time asset dependency graph (DAG).
///
/// Maintains directed edges: "A depends on B" means A is the dependent and B is
/// the dependency. If B changes, A must be recooked.
///
/// This is a build tool, not per-frame engine code, so std containers are fine.
struct DependencyGraph final {
  using AssetId = std::uint64_t;

  static constexpr AssetId kInvalidAssetId = 0ULL;

  /// Forward edges: asset -> set of assets it depends on.
  std::unordered_map<AssetId, std::unordered_set<AssetId>> dependencies{};

  /// Reverse edges: asset -> set of assets that depend on it.
  std::unordered_map<AssetId, std::unordered_set<AssetId>> dependents{};

  /// Optional path mapping for human-readable serialization.
  std::unordered_map<AssetId, std::string> assetPaths{};
};

/// Reset the graph to empty state.
inline void clear_dependency_graph(DependencyGraph *graph) noexcept {
  if (graph == nullptr) {
    return;
  }
  graph->dependencies.clear();
  graph->dependents.clear();
  graph->assetPaths.clear();
}

/// Register a human-readable path for an asset ID.
inline void register_asset_path(DependencyGraph *graph,
                                DependencyGraph::AssetId id,
                                const char *path) noexcept {
  if ((graph == nullptr) || (id == DependencyGraph::kInvalidAssetId) ||
      (path == nullptr)) {
    return;
  }
  graph->assetPaths[id] = path;
}

/// Add edge: `dependent` depends on `dependency`.
/// Returns false if either ID is invalid or if the edge would create a
/// self-loop.
bool add_dependency(DependencyGraph *graph, DependencyGraph::AssetId dependent,
                    DependencyGraph::AssetId dependency) noexcept;

/// Remove a single dependency edge.
bool remove_dependency(DependencyGraph *graph,
                       DependencyGraph::AssetId dependent,
                       DependencyGraph::AssetId dependency) noexcept;

/// Remove all edges involving this asset (both as dependent and dependency).
void remove_asset(DependencyGraph *graph, DependencyGraph::AssetId id) noexcept;

/// Query: what does `id` depend on? (forward edges)
/// Returns count written to outIds. Pass nullptr/0 to just get the count.
std::size_t get_dependencies(const DependencyGraph *graph,
                             DependencyGraph::AssetId id,
                             DependencyGraph::AssetId *outIds,
                             std::size_t maxIds) noexcept;

/// Query: what depends on `id`? (reverse edges — what needs recook if id
/// changes)
std::size_t get_dependents(const DependencyGraph *graph,
                           DependencyGraph::AssetId id,
                           DependencyGraph::AssetId *outIds,
                           std::size_t maxIds) noexcept;

/// Recursively collect all assets that transitively depend on `id`.
/// This is the full invalidation set when `id` changes.
std::size_t get_all_dependents_recursive(const DependencyGraph *graph,
                                         DependencyGraph::AssetId id,
                                         DependencyGraph::AssetId *outIds,
                                         std::size_t maxIds) noexcept;

/// Check whether adding edge (dependent -> dependency) would create a cycle.
/// Returns true if a cycle would be introduced.
bool would_create_cycle(const DependencyGraph *graph,
                        DependencyGraph::AssetId dependent,
                        DependencyGraph::AssetId dependency) noexcept;

/// Check if the graph contains any cycles.
bool has_cycle(const DependencyGraph *graph) noexcept;

/// Produce a topological ordering of all assets in the graph.
/// Returns number of assets written. If there is a cycle, returns 0.
std::size_t topological_sort(const DependencyGraph *graph,
                             DependencyGraph::AssetId *outIds,
                             std::size_t maxIds) noexcept;

/// Serialize graph to JSON file at `path`.
bool write_dependency_graph_json(const DependencyGraph *graph,
                                 const char *path) noexcept;

/// Deserialize graph from JSON file at `path`.
bool read_dependency_graph_json(DependencyGraph *graph,
                                const char *path) noexcept;

/// Given a set of changed asset IDs, return all assets that need recooked
/// (transitive dependents). The changed assets themselves are NOT included.
std::size_t compute_invalidation_set(const DependencyGraph *graph,
                                     const DependencyGraph::AssetId *changedIds,
                                     std::size_t changedCount,
                                     DependencyGraph::AssetId *outIds,
                                     std::size_t maxIds) noexcept;

} // namespace engine::tools
