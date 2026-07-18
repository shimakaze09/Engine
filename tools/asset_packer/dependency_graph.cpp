// Implements dependency graph behavior for the Engine tooling.

#include "dependency_graph.h"

#include "engine/core/json.h"

#include <algorithm>
#include <queue>
#include <stack>
#include <utility>
#include <vector>

namespace engine::tools {
namespace {

bool write_text_file(const char *path, const char *text,
                     std::size_t textSize) noexcept {
  if ((path == nullptr) || (text == nullptr)) {
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

  const bool ok = (std::fwrite(text, 1U, textSize, file) == textSize);
  std::fclose(file);
  if (!ok) {
    std::fprintf(stderr, "error: failed to write %s\n", path);
  }
  return ok;
}

bool read_text_file(const char *path, std::string *out) noexcept {
  if ((path == nullptr) || (out == nullptr)) {
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

  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (fileSize <= 0) {
    std::fclose(file);
    return false;
  }

  std::string content(static_cast<std::size_t>(fileSize), '\0');
  const std::size_t readBytes =
      std::fread(content.data(), 1U, content.size(), file);
  std::fclose(file);
  if (readBytes != content.size()) {
    return false;
  }

  *out = std::move(content);
  return true;
}

void format_asset_id(DependencyGraph::AssetId id, char (&out)[17]) noexcept {
  std::snprintf(out, 17U, "%016llx", static_cast<unsigned long long>(id));
}

void write_asset_id_string(engine::core::JsonWriter &writer, const char *key,
                           DependencyGraph::AssetId id) noexcept {
  char text[17] = {};
  format_asset_id(id, text);
  writer.write_string(key, text);
}

bool copy_json_string(const engine::core::JsonParser &parser,
                      const engine::core::JsonValue &value,
                      std::string *out) noexcept {
  if (out == nullptr) {
    return false;
  }

  const char *rawBegin = nullptr;
  std::size_t rawLength = 0U;
  if (!parser.as_string(value, &rawBegin, &rawLength)) {
    return false;
  }

  std::vector<char> buffer(rawLength + 1U, '\0');
  if (!parser.copy_string(value, buffer.data(), buffer.size())) {
    return false;
  }

  *out = buffer.data();
  return true;
}

bool read_string_field(const engine::core::JsonParser &parser,
                       const engine::core::JsonValue &object,
                       const char *fieldName, std::string *out) noexcept {
  engine::core::JsonValue value{};
  if (!parser.get_object_field(object, fieldName, &value)) {
    return false;
  }
  return copy_json_string(parser, value, out);
}

bool parse_asset_id(const std::string &text,
                    DependencyGraph::AssetId *out) noexcept {
  if ((out == nullptr) || text.empty()) {
    return false;
  }

  unsigned long long value = 0ULL;
  char extra = '\0';
  if (std::sscanf(text.c_str(), "%llx%c", &value, &extra) != 1) {
    return false;
  }

  *out = static_cast<DependencyGraph::AssetId>(value);
  return true;
}

bool read_asset_id_field(const engine::core::JsonParser &parser,
                         const engine::core::JsonValue &object,
                         const char *fieldName,
                         DependencyGraph::AssetId *out) noexcept {
  std::string text{};
  return read_string_field(parser, object, fieldName, &text) &&
         parse_asset_id(text, out);
}

} // namespace

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

/// Returns whether has cycle.
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

/// Converts topological sort into the target representation.
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

  std::vector<std::pair<DependencyGraph::AssetId, DependencyGraph::AssetId>>
      edges{};
  for (const auto &[dependent, deps] : graph->dependencies) {
    for (const auto dependency : deps) {
      edges.emplace_back(dependent, dependency);
    }
  }
  std::sort(edges.begin(), edges.end());

  std::vector<std::pair<DependencyGraph::AssetId, std::string>> assets{};
  assets.reserve(graph->assetPaths.size());
  for (const auto &[id, assetPath] : graph->assetPaths) {
    assets.emplace_back(id, assetPath);
  }
  std::sort(assets.begin(), assets.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  engine::core::JsonWriter writer{};
  writer.begin_object();
  writer.write_uint("schemaVersion", 1U);

  writer.begin_array("edges");
  for (const auto &[dependent, dependency] : edges) {
    const auto dependentPath = graph->assetPaths.find(dependent);
    const auto dependencyPath = graph->assetPaths.find(dependency);

    writer.begin_object();
    write_asset_id_string(writer, "dependent", dependent);
    write_asset_id_string(writer, "dependency", dependency);
    writer.write_string("dependentPath",
                        dependentPath != graph->assetPaths.end()
                            ? dependentPath->second.c_str()
                            : "");
    writer.write_string("dependencyPath",
                        dependencyPath != graph->assetPaths.end()
                            ? dependencyPath->second.c_str()
                            : "");
    writer.end_object();
  }
  writer.end_array();

  writer.begin_array("assets");
  for (const auto &[id, assetPath] : assets) {
    writer.begin_object();
    write_asset_id_string(writer, "id", id);
    writer.write_string("path", assetPath.c_str());
    writer.end_object();
  }
  writer.end_array();

  writer.end_object();
  if (!writer.ok()) {
    std::fprintf(stderr, "error: failed to serialize dependency graph\n");
    return false;
  }

  return write_text_file(path, writer.result(), writer.result_size());
}

/// Reads dependency graph json data.
bool read_dependency_graph_json(DependencyGraph *graph,
                                const char *path) noexcept {
  if ((graph == nullptr) || (path == nullptr)) {
    return false;
  }

  std::string content{};
  if (!read_text_file(path, &content)) {
    return false;
  }

  engine::core::JsonParser parser{};
  if (!parser.parse(content.data(), content.size())) {
    return false;
  }

  const engine::core::JsonValue *root = parser.root();
  if ((root == nullptr) ||
      (root->type != engine::core::JsonValue::Type::Object)) {
    return false;
  }

  DependencyGraph loaded{};

  engine::core::JsonValue assets{};
  if (parser.get_object_field(*root, "assets", &assets)) {
    if (assets.type != engine::core::JsonValue::Type::Array) {
      return false;
    }

    const std::size_t assetCount = parser.array_size(assets);
    for (std::size_t i = 0U; i < assetCount; ++i) {
      engine::core::JsonValue asset{};
      if (!parser.get_array_element(assets, i, &asset) ||
          (asset.type != engine::core::JsonValue::Type::Object)) {
        return false;
      }
      DependencyGraph::AssetId id = DependencyGraph::kInvalidAssetId;
      std::string assetPath{};
      if (!read_asset_id_field(parser, asset, "id", &id) ||
          !read_string_field(parser, asset, "path", &assetPath)) {
        return false;
      }

      loaded.assetPaths[id] = assetPath;
    }
  }

  engine::core::JsonValue edges{};
  if (!parser.get_object_field(*root, "edges", &edges)) {
    *graph = std::move(loaded);
    return true;
  }
  if (edges.type != engine::core::JsonValue::Type::Array) {
    return false;
  }

  const std::size_t edgeCount = parser.array_size(edges);
  for (std::size_t i = 0U; i < edgeCount; ++i) {
    engine::core::JsonValue edge{};
    if (!parser.get_array_element(edges, i, &edge) ||
        (edge.type != engine::core::JsonValue::Type::Object)) {
      return false;
    }

    DependencyGraph::AssetId dependent = DependencyGraph::kInvalidAssetId;
    DependencyGraph::AssetId dependency = DependencyGraph::kInvalidAssetId;
    if (!read_asset_id_field(parser, edge, "dependent", &dependent) ||
        !read_asset_id_field(parser, edge, "dependency", &dependency)) {
      return false;
    }

    loaded.dependencies[dependent].insert(dependency);
    loaded.dependents[dependency].insert(dependent);
  }

  *graph = std::move(loaded);
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
