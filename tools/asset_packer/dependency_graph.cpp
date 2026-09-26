// Implements dependency graph behavior for the Engine tooling.

#include "dependency_graph.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <stack>
#include <utility>
#include <vector>

namespace engine::tools {
namespace {

/// Copies an id set ascending so callers see a deterministic order and
/// truncation keeps the smallest ids.
std::size_t copy_ids_sorted(
    const std::unordered_set<DependencyGraph::AssetId> &ids,
    DependencyGraph::AssetId *outIds, std::size_t maxIds) noexcept {
  std::vector<DependencyGraph::AssetId> sortedIds(ids.begin(), ids.end());
  std::sort(sortedIds.begin(), sortedIds.end());

  std::size_t count = 0U;
  for (const auto id : sortedIds) {
    if (count >= maxIds) {
      break;
    }
    outIds[count] = id;
    ++count;
  }
  return count;
}

} // namespace

bool add_dependency(DependencyGraph *graph, DependencyGraph::AssetId dependent,
                    DependencyGraph::AssetId dependency) noexcept {
  if ((graph == nullptr) || (dependent == DependencyGraph::kInvalidAssetId) ||
      (dependency == DependencyGraph::kInvalidAssetId) ||
      (dependent == dependency)) {
    return false;
  }

  if (would_create_cycle(graph, dependent, dependency)) {
    std::fprintf(stderr,
                 "error: adding dependency %016llx -> %016llx would create a "
                 "cycle\n",
                 static_cast<unsigned long long>(dependent),
                 static_cast<unsigned long long>(dependency));
    return false;
  }

  graph->dependencies[dependent].insert(dependency);
  graph->dependents[dependency].insert(dependent);
  return true;
}

bool remove_dependency(DependencyGraph *graph,
                       DependencyGraph::AssetId dependent,
                       DependencyGraph::AssetId dependency) noexcept {
  if ((graph == nullptr) || (dependent == DependencyGraph::kInvalidAssetId) ||
      (dependency == DependencyGraph::kInvalidAssetId)) {
    return false;
  }

  auto depIt = graph->dependencies.find(dependent);
  if (depIt == graph->dependencies.end()) {
    return false;
  }

  const auto erased = depIt->second.erase(dependency);
  if (erased == 0U) {
    return false;
  }

  if (depIt->second.empty()) {
    graph->dependencies.erase(depIt);
  }

  auto revIt = graph->dependents.find(dependency);
  if (revIt != graph->dependents.end()) {
    revIt->second.erase(dependent);
    if (revIt->second.empty()) {
      graph->dependents.erase(revIt);
    }
  }

  return true;
}

void remove_asset(DependencyGraph *graph,
                  DependencyGraph::AssetId id) noexcept {
  if ((graph == nullptr) || (id == DependencyGraph::kInvalidAssetId)) {
    return;
  }

  auto fwdIt = graph->dependencies.find(id);
  if (fwdIt != graph->dependencies.end()) {
    for (const auto dep : fwdIt->second) {
      auto revIt = graph->dependents.find(dep);
      if (revIt != graph->dependents.end()) {
        revIt->second.erase(id);
        if (revIt->second.empty()) {
          graph->dependents.erase(revIt);
        }
      }
    }
    graph->dependencies.erase(fwdIt);
  }

  auto revIt = graph->dependents.find(id);
  if (revIt != graph->dependents.end()) {
    for (const auto dep : revIt->second) {
      auto fwdInner = graph->dependencies.find(dep);
      if (fwdInner != graph->dependencies.end()) {
        fwdInner->second.erase(id);
        if (fwdInner->second.empty()) {
          graph->dependencies.erase(fwdInner);
        }
      }
    }
    graph->dependents.erase(revIt);
  }

  graph->assetPaths.erase(id);
}

std::size_t get_dependencies(const DependencyGraph *graph,
                             DependencyGraph::AssetId id,
                             DependencyGraph::AssetId *outIds,
                             std::size_t maxIds) noexcept {
  if ((graph == nullptr) || (id == DependencyGraph::kInvalidAssetId)) {
    return 0U;
  }

  auto it = graph->dependencies.find(id);
  if (it == graph->dependencies.end()) {
    return 0U;
  }

  if ((outIds == nullptr) || (maxIds == 0U)) {
    return it->second.size();
  }

  return copy_ids_sorted(it->second, outIds, maxIds);
}

std::size_t get_dependents(const DependencyGraph *graph,
                           DependencyGraph::AssetId id,
                           DependencyGraph::AssetId *outIds,
                           std::size_t maxIds) noexcept {
  if ((graph == nullptr) || (id == DependencyGraph::kInvalidAssetId)) {
    return 0U;
  }

  auto it = graph->dependents.find(id);
  if (it == graph->dependents.end()) {
    return 0U;
  }

  if ((outIds == nullptr) || (maxIds == 0U)) {
    return it->second.size();
  }

  return copy_ids_sorted(it->second, outIds, maxIds);
}

/// BFS over reverse edges from the asset's direct dependents.
std::size_t get_all_dependents_recursive(const DependencyGraph *graph,
                                         DependencyGraph::AssetId id,
                                         DependencyGraph::AssetId *outIds,
                                         std::size_t maxIds) noexcept {
  if ((graph == nullptr) || (id == DependencyGraph::kInvalidAssetId) ||
      (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  std::unordered_set<DependencyGraph::AssetId> visited{};
  std::queue<DependencyGraph::AssetId> frontier{};

  auto it = graph->dependents.find(id);
  if (it != graph->dependents.end()) {
    for (const auto dep : it->second) {
      if (visited.insert(dep).second) {
        frontier.push(dep);
      }
    }
  }

  while (!frontier.empty()) {
    const auto current = frontier.front();
    frontier.pop();

    auto revIt = graph->dependents.find(current);
    if (revIt != graph->dependents.end()) {
      for (const auto dep : revIt->second) {
        if (visited.insert(dep).second) {
          frontier.push(dep);
        }
      }
    }
  }

  return copy_ids_sorted(visited, outIds, maxIds);
}

/// A cycle would exist if the dependency already transitively depends on
/// the dependent — BFS from dependency along forward edges looking for
/// the dependent.
bool would_create_cycle(const DependencyGraph *graph,
                        DependencyGraph::AssetId dependent,
                        DependencyGraph::AssetId dependency) noexcept {
  if ((graph == nullptr) || (dependent == DependencyGraph::kInvalidAssetId) ||
      (dependency == DependencyGraph::kInvalidAssetId)) {
    return false;
  }

  if (dependent == dependency) {
    return true;
  }

  std::unordered_set<DependencyGraph::AssetId> visited{};
  std::queue<DependencyGraph::AssetId> frontier{};
  frontier.push(dependency);
  visited.insert(dependency);

  while (!frontier.empty()) {
    const auto current = frontier.front();
    frontier.pop();

    auto it = graph->dependencies.find(current);
    if (it == graph->dependencies.end()) {
      continue;
    }

    for (const auto dep : it->second) {
      if (dep == dependent) {
        return true;
      }
      if (visited.insert(dep).second) {
        frontier.push(dep);
      }
    }
  }

  return false;
}

/// Returns whether has cycle.
/// Kahn's algorithm: if the topological peel cannot process every node,
/// a cycle exists.
bool has_cycle(const DependencyGraph *graph) noexcept {
  if (graph == nullptr) {
    return false;
  }

  std::unordered_set<DependencyGraph::AssetId> allNodes{};
  for (const auto &[key, _] : graph->dependencies) {
    allNodes.insert(key);
  }
  for (const auto &[key, _] : graph->dependents) {
    allNodes.insert(key);
  }

  std::unordered_map<DependencyGraph::AssetId, std::size_t> inDegree{};
  for (const auto node : allNodes) {
    inDegree[node] = 0U;
  }
  for (const auto &[node, deps] : graph->dependencies) {
    inDegree[node] = deps.size();
  }

  std::queue<DependencyGraph::AssetId> ready{};
  for (const auto &[node, deg] : inDegree) {
    if (deg == 0U) {
      ready.push(node);
    }
  }

  std::size_t processed = 0U;
  while (!ready.empty()) {
    const auto current = ready.front();
    ready.pop();
    ++processed;

    auto revIt = graph->dependents.find(current);
    if (revIt == graph->dependents.end()) {
      continue;
    }

    for (const auto dep : revIt->second) {
      auto &deg = inDegree[dep];
      if (deg > 0U) {
        --deg;
      }
      if (deg == 0U) {
        ready.push(dep);
      }
    }
  }

  return processed < allNodes.size();
}

/// Converts topological sort into the target representation.
/// Kahn's algorithm over all nodes (including leaf dependencies that
/// appear only as edge targets), dependencies before dependents; the
/// min-heap emits ready ties smallest id first so the order is unique
/// and deterministic.
std::size_t topological_sort(const DependencyGraph *graph,
                             DependencyGraph::AssetId *outIds,
                             std::size_t maxIds) noexcept {
  if ((graph == nullptr) || (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  std::unordered_set<DependencyGraph::AssetId> allNodes{};
  for (const auto &[key, _] : graph->dependencies) {
    allNodes.insert(key);
  }
  for (const auto &[key, _] : graph->dependents) {
    allNodes.insert(key);
  }

  for (const auto &[_, deps] : graph->dependencies) {
    for (const auto d : deps) {
      allNodes.insert(d);
    }
  }

  std::unordered_map<DependencyGraph::AssetId, std::size_t> inDegree{};
  for (const auto node : allNodes) {
    inDegree[node] = 0U;
  }
  for (const auto &[node, deps] : graph->dependencies) {
    inDegree[node] = deps.size();
  }

  std::priority_queue<DependencyGraph::AssetId,
                      std::vector<DependencyGraph::AssetId>,
                      std::greater<DependencyGraph::AssetId>>
      ready{};
  for (const auto &[node, deg] : inDegree) {
    if (deg == 0U) {
      ready.push(node);
    }
  }

  std::size_t count = 0U;
  while (!ready.empty()) {
    const auto current = ready.top();
    ready.pop();

    if (count >= maxIds) {
      break;
    }
    outIds[count] = current;
    ++count;

    auto revIt = graph->dependents.find(current);
    if (revIt == graph->dependents.end()) {
      continue;
    }

    for (const auto dep : revIt->second) {
      auto &deg = inDegree[dep];
      if (deg > 0U) {
        --deg;
      }
      if (deg == 0U) {
        ready.push(dep);
      }
    }
  }

  if (count < allNodes.size()) {
    return 0U;
  }

  return count;
}

/// BFS from every changed asset over reverse edges: the transitive set
/// of dependents that must recook.
std::size_t compute_invalidation_set(const DependencyGraph *graph,
                                     const DependencyGraph::AssetId *changedIds,
                                     std::size_t changedCount,
                                     DependencyGraph::AssetId *outIds,
                                     std::size_t maxIds) noexcept {
  if ((graph == nullptr) || (changedIds == nullptr) || (changedCount == 0U) ||
      (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  std::unordered_set<DependencyGraph::AssetId> changedSet(
      changedIds, changedIds + changedCount);
  std::unordered_set<DependencyGraph::AssetId> invalidated{};
  std::queue<DependencyGraph::AssetId> frontier{};

  for (std::size_t i = 0U; i < changedCount; ++i) {
    auto it = graph->dependents.find(changedIds[i]);
    if (it != graph->dependents.end()) {
      for (const auto dep : it->second) {
        if ((changedSet.find(dep) == changedSet.end()) &&
            invalidated.insert(dep).second) {
          frontier.push(dep);
        }
      }
    }
  }

  while (!frontier.empty()) {
    const auto current = frontier.front();
    frontier.pop();

    auto it = graph->dependents.find(current);
    if (it == graph->dependents.end()) {
      continue;
    }

    for (const auto dep : it->second) {
      if ((changedSet.find(dep) == changedSet.end()) &&
          invalidated.insert(dep).second) {
        frontier.push(dep);
      }
    }
  }

  return copy_ids_sorted(invalidated, outIds, maxIds);
}

} // namespace engine::tools
