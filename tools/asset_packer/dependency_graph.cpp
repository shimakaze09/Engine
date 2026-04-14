#include "dependency_graph.h"

#include <algorithm>
#include <queue>
#include <stack>

namespace engine::tools {

bool add_dependency(DependencyGraph *graph, DependencyGraph::AssetId dependent,
                    DependencyGraph::AssetId dependency) noexcept {
  if ((graph == nullptr) || (dependent == DependencyGraph::kInvalidAssetId) ||
      (dependency == DependencyGraph::kInvalidAssetId) ||
      (dependent == dependency)) {
    return false;
  }

  // Check for cycles before adding.
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

  // Remove forward edges (things id depends on).
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

  // Remove reverse edges (things that depend on id).
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

  std::size_t count = 0U;
  for (const auto dep : it->second) {
    if (count >= maxIds) {
      break;
    }
    outIds[count] = dep;
    ++count;
  }
  return count;
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

  std::size_t count = 0U;
  for (const auto dep : it->second) {
    if (count >= maxIds) {
      break;
    }
    outIds[count] = dep;
    ++count;
  }
  return count;
}

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

  // Seed with direct dependents.
  auto it = graph->dependents.find(id);
  if (it != graph->dependents.end()) {
    for (const auto dep : it->second) {
      if (visited.insert(dep).second) {
        frontier.push(dep);
      }
    }
  }

  // BFS through reverse edges.
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

  std::size_t count = 0U;
  for (const auto dep : visited) {
    if (count >= maxIds) {
      break;
    }
    outIds[count] = dep;
    ++count;
  }
  return count;
}

bool would_create_cycle(const DependencyGraph *graph,
                        DependencyGraph::AssetId dependent,
                        DependencyGraph::AssetId dependency) noexcept {
  if ((graph == nullptr) || (dependent == DependencyGraph::kInvalidAssetId) ||
      (dependency == DependencyGraph::kInvalidAssetId)) {
    return false;
  }

  // A cycle exists if there is already a path from dependent to dependency
  // in the reverse direction, i.e., dependency transitively depends on
  // dependent.
  if (dependent == dependency) {
    return true;
  }

  // BFS from dependency following forward edges; if we reach dependent, cycle.
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

bool has_cycle(const DependencyGraph *graph) noexcept {
  if (graph == nullptr) {
    return false;
  }

  // Collect all nodes.
  std::unordered_set<DependencyGraph::AssetId> allNodes{};
  for (const auto &[key, _] : graph->dependencies) {
    allNodes.insert(key);
  }
  for (const auto &[key, _] : graph->dependents) {
    allNodes.insert(key);
  }

  // Kahn's algorithm: compute in-degree and do topological peel.
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

std::size_t topological_sort(const DependencyGraph *graph,
                             DependencyGraph::AssetId *outIds,
                             std::size_t maxIds) noexcept {
  if ((graph == nullptr) || (outIds == nullptr) || (maxIds == 0U)) {
    return 0U;
  }

  // Collect all nodes.
  std::unordered_set<DependencyGraph::AssetId> allNodes{};
  for (const auto &[key, _] : graph->dependencies) {
    allNodes.insert(key);
  }
  for (const auto &[key, _] : graph->dependents) {
    allNodes.insert(key);
  }

  // Also include leaf dependencies that appear only as values.
  for (const auto &[_, deps] : graph->dependencies) {
    for (const auto d : deps) {
      allNodes.insert(d);
    }
  }

  // Kahn's algorithm.
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

  std::size_t count = 0U;
  while (!ready.empty()) {
    const auto current = ready.front();
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

  // If not all nodes were processed, there is a cycle.
  if (count < allNodes.size()) {
    return 0U;
  }

  return count;
}

// --- JSON serialization ---

bool write_dependency_graph_json(const DependencyGraph *graph,
                                 const char *path) noexcept {
  if ((graph == nullptr) || (path == nullptr)) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    std::fprintf(stderr, "error: cannot open %s for writing\n", path);
    return false;
  }

  std::fprintf(file, "{\n  \"schemaVersion\": 1,\n  \"edges\": [\n");

  bool firstEdge = true;
  for (const auto &[dependent, deps] : graph->dependencies) {
    for (const auto dependency : deps) {
      if (!firstEdge) {
        std::fprintf(file, ",\n");
      }
      firstEdge = false;

      const char *depPath = "";
      const char *depyPath = "";
      auto pathIt = graph->assetPaths.find(dependent);
      if (pathIt != graph->assetPaths.end()) {
        depPath = pathIt->second.c_str();
      }
      auto pathIt2 = graph->assetPaths.find(dependency);
      if (pathIt2 != graph->assetPaths.end()) {
        depyPath = pathIt2->second.c_str();
      }

      std::fprintf(file,
                   "    { \"dependent\": \"%016llx\", \"dependency\": "
                   "\"%016llx\", \"dependentPath\": \"%s\", "
                   "\"dependencyPath\": \"%s\" }",
                   static_cast<unsigned long long>(dependent),
                   static_cast<unsigned long long>(dependency), depPath,
                   depyPath);
    }
  }

  std::fprintf(file, "\n  ],\n  \"assets\": [\n");

  bool firstAsset = true;
  for (const auto &[id, assetPath] : graph->assetPaths) {
    if (!firstAsset) {
      std::fprintf(file, ",\n");
    }
    firstAsset = false;
    std::fprintf(file, "    { \"id\": \"%016llx\", \"path\": \"%s\" }",
                 static_cast<unsigned long long>(id), assetPath.c_str());
  }

  std::fprintf(file, "\n  ]\n}\n");
  std::fclose(file);
  return true;
}

bool read_dependency_graph_json(DependencyGraph *graph,
                                const char *path) noexcept {
  if ((graph == nullptr) || (path == nullptr)) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  // Read entire file into string.
  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);

  if (fileSize <= 0) {
    std::fclose(file);
    return false;
  }

  std::string content(static_cast<std::size_t>(fileSize), '\0');
  const std::size_t readBytes =
      std::fread(content.data(), 1U, static_cast<std::size_t>(fileSize), file);
  std::fclose(file);

  if (readBytes != static_cast<std::size_t>(fileSize)) {
    return false;
  }

  clear_dependency_graph(graph);

  // Minimal JSON parser: extract edges and asset paths.
  // Parse edge entries: { "dependent": "hex", "dependency": "hex", ... }
  const char *cursor = content.c_str();

  // Parse assets section first for path mapping.
  const char *assetsSection = std::strstr(cursor, "\"assets\"");
  if (assetsSection != nullptr) {
    const char *pos = assetsSection;
    while ((pos = std::strstr(pos, "\"id\"")) != nullptr) {
      // Find the hex string after "id": "
      const char *idStart = std::strchr(pos, ':');
      if (idStart == nullptr) {
        break;
      }
      idStart = std::strchr(idStart, '"');
      if (idStart == nullptr) {
        break;
      }
      ++idStart; // skip opening quote
      unsigned long long idVal = 0ULL;
      if (std::sscanf(idStart, "%llx", &idVal) != 1) {
        ++pos;
        continue;
      }

      const char *pathKey = std::strstr(idStart, "\"path\"");
      if (pathKey == nullptr) {
        ++pos;
        continue;
      }
      const char *pathValStart = std::strchr(pathKey + 6, '"');
      if (pathValStart == nullptr) {
        ++pos;
        continue;
      }
      ++pathValStart; // skip opening quote
      const char *pathValEnd = std::strchr(pathValStart, '"');
      if (pathValEnd == nullptr) {
        ++pos;
        continue;
      }

      std::string assetPath(
          pathValStart, static_cast<std::size_t>(pathValEnd - pathValStart));
      graph->assetPaths[static_cast<DependencyGraph::AssetId>(idVal)] =
          assetPath;
      pos = pathValEnd + 1;
    }
  }

  // Parse edges section.
  const char *edgesSection = std::strstr(cursor, "\"edges\"");
  if (edgesSection == nullptr) {
    return true; // Empty graph is valid.
  }

  const char *pos = edgesSection;
  while ((pos = std::strstr(pos, "\"dependent\"")) != nullptr) {
    const char *depStart = std::strchr(pos, ':');
    if (depStart == nullptr) {
      break;
    }
    depStart = std::strchr(depStart, '"');
    if (depStart == nullptr) {
      break;
    }
    ++depStart;
    unsigned long long dependentVal = 0ULL;
    if (std::sscanf(depStart, "%llx", &dependentVal) != 1) {
      ++pos;
      continue;
    }

    const char *depyKey = std::strstr(depStart, "\"dependency\"");
    if (depyKey == nullptr) {
      ++pos;
      continue;
    }
    const char *depyStart = std::strchr(depyKey + 12, '"');
    if (depyStart == nullptr) {
      ++pos;
      continue;
    }
    ++depyStart;
    unsigned long long dependencyVal = 0ULL;
    if (std::sscanf(depyStart, "%llx", &dependencyVal) != 1) {
      ++pos;
      continue;
    }

    const auto dependent = static_cast<DependencyGraph::AssetId>(dependentVal);
    const auto dependency =
        static_cast<DependencyGraph::AssetId>(dependencyVal);

    // Directly insert without cycle check — trusting persisted data.
    graph->dependencies[dependent].insert(dependency);
    graph->dependents[dependency].insert(dependent);

    pos = depyStart + 1;
  }

  return true;
}

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

  // Seed BFS with direct dependents of all changed assets.
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

  // BFS to find transitive dependents.
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

  std::size_t count = 0U;
  for (const auto id : invalidated) {
    if (count >= maxIds) {
      break;
    }
    outIds[count] = id;
    ++count;
  }
  return count;
}

} // namespace engine::tools
