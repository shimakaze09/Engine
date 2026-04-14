#include <cstdio>
#include <cstdlib>
#include <cstring>

// Include the dependency graph header directly from the tools directory.
// The test links against the dependency_graph.cpp object.
#include "dependency_graph.h"

namespace {

// Helper: create a temp file path for graph serialization tests.
bool make_temp_graph_path(char *outPath, std::size_t outSize) {
#ifdef _WIN32
  const char *tmpDir = std::getenv("TEMP");
  if (tmpDir == nullptr) {
    tmpDir = ".";
  }
  return std::snprintf(outPath, outSize, "%s/dep_graph_test.json", tmpDir) > 0;
#else
  return std::snprintf(outPath, outSize, "/tmp/dep_graph_test.json") > 0;
#endif
}

} // namespace

int main() {
  using Graph = engine::tools::DependencyGraph;
  using AssetId = Graph::AssetId;

  // --- Test 1: Basic add/query dependencies ---
  {
    Graph graph{};
    constexpr AssetId kMesh = 100ULL;
    constexpr AssetId kTexture = 200ULL;
    constexpr AssetId kMaterial = 300ULL;

    // mesh depends on material, material depends on texture.
    if (!engine::tools::add_dependency(&graph, kMesh, kMaterial)) {
      return 1;
    }
    if (!engine::tools::add_dependency(&graph, kMaterial, kTexture)) {
      return 2;
    }

    // Query forward deps of mesh: should be {material}.
    AssetId deps[8] = {};
    std::size_t count = engine::tools::get_dependencies(&graph, kMesh, deps, 8);
    if (count != 1U) {
      return 3;
    }
    if (deps[0] != kMaterial) {
      return 4;
    }

    // Query forward deps of material: should be {texture}.
    count = engine::tools::get_dependencies(&graph, kMaterial, deps, 8);
    if (count != 1U) {
      return 5;
    }
    if (deps[0] != kTexture) {
      return 6;
    }

    // Query forward deps of texture: should be empty.
    count = engine::tools::get_dependencies(&graph, kTexture, deps, 8);
    if (count != 0U) {
      return 7;
    }
  }

  // --- Test 2: Reverse dependency query (dependents) ---
  {
    Graph graph{};
    constexpr AssetId kMesh = 100ULL;
    constexpr AssetId kTexture = 200ULL;
    constexpr AssetId kMaterial = 300ULL;

    engine::tools::add_dependency(&graph, kMesh, kMaterial);
    engine::tools::add_dependency(&graph, kMaterial, kTexture);

    // What depends on texture? -> material.
    AssetId deps[8] = {};
    std::size_t count =
        engine::tools::get_dependents(&graph, kTexture, deps, 8);
    if (count != 1U) {
      return 10;
    }
    if (deps[0] != kMaterial) {
      return 11;
    }

    // What depends on material? -> mesh.
    count = engine::tools::get_dependents(&graph, kMaterial, deps, 8);
    if (count != 1U) {
      return 12;
    }
    if (deps[0] != kMesh) {
      return 13;
    }
  }

  // --- Test 3: Recursive dependents (invalidation set) ---
  {
    Graph graph{};
    constexpr AssetId kMesh = 100ULL;
    constexpr AssetId kTexture = 200ULL;
    constexpr AssetId kMaterial = 300ULL;

    engine::tools::add_dependency(&graph, kMesh, kMaterial);
    engine::tools::add_dependency(&graph, kMaterial, kTexture);

    // All transitive dependents of texture: {material, mesh}.
    AssetId deps[8] = {};
    const std::size_t count =
        engine::tools::get_all_dependents_recursive(&graph, kTexture, deps, 8);
    if (count != 2U) {
      return 20;
    }

    // Both mesh and material should be in the set.
    bool foundMesh = false;
    bool foundMaterial = false;
    for (std::size_t i = 0U; i < count; ++i) {
      if (deps[i] == kMesh) {
        foundMesh = true;
      }
      if (deps[i] == kMaterial) {
        foundMaterial = true;
      }
    }
    if (!foundMesh || !foundMaterial) {
      return 21;
    }
  }

  // --- Test 4: Self-loop rejected ---
  {
    Graph graph{};
    if (engine::tools::add_dependency(&graph, 1ULL, 1ULL)) {
      return 30;
    }
  }

  // --- Test 5: Cycle detection ---
  {
    Graph graph{};
    // A->B->C, then try to add C->A (cycle).
    engine::tools::add_dependency(&graph, 1ULL, 2ULL);
    engine::tools::add_dependency(&graph, 2ULL, 3ULL);

    if (engine::tools::add_dependency(&graph, 3ULL, 1ULL)) {
      return 40; // Should have been rejected.
    }

    if (engine::tools::has_cycle(&graph)) {
      return 41; // Graph should be cycle-free.
    }
  }

  // --- Test 6: would_create_cycle ---
  {
    Graph graph{};
    engine::tools::add_dependency(&graph, 1ULL, 2ULL);
    engine::tools::add_dependency(&graph, 2ULL, 3ULL);

    if (!engine::tools::would_create_cycle(&graph, 3ULL, 1ULL)) {
      return 50;
    }

    // Adding 1->4 should not create a cycle.
    if (engine::tools::would_create_cycle(&graph, 1ULL, 4ULL)) {
      return 51;
    }
  }

  // --- Test 7: Topological sort ---
  {
    Graph graph{};
    constexpr AssetId kA = 10ULL;
    constexpr AssetId kB = 20ULL;
    constexpr AssetId kC = 30ULL;

    // A depends on B, B depends on C.
    engine::tools::add_dependency(&graph, kA, kB);
    engine::tools::add_dependency(&graph, kB, kC);

    AssetId sorted[8] = {};
    const std::size_t count =
        engine::tools::topological_sort(&graph, sorted, 8);
    if (count != 3U) {
      return 60;
    }

    // C must come before B, B must come before A.
    std::size_t posC = 999U;
    std::size_t posB = 999U;
    std::size_t posA = 999U;
    for (std::size_t i = 0U; i < count; ++i) {
      if (sorted[i] == kC) {
        posC = i;
      }
      if (sorted[i] == kB) {
        posB = i;
      }
      if (sorted[i] == kA) {
        posA = i;
      }
    }
    if ((posC >= posB) || (posB >= posA)) {
      return 61;
    }
  }

  // --- Test 8: Remove dependency ---
  {
    Graph graph{};
    engine::tools::add_dependency(&graph, 1ULL, 2ULL);
    engine::tools::add_dependency(&graph, 1ULL, 3ULL);

    if (!engine::tools::remove_dependency(&graph, 1ULL, 2ULL)) {
      return 70;
    }

    AssetId deps[8] = {};
    const std::size_t count =
        engine::tools::get_dependencies(&graph, 1ULL, deps, 8);
    if (count != 1U) {
      return 71;
    }
    if (deps[0] != 3ULL) {
      return 72;
    }
  }

  // --- Test 9: Remove asset ---
  {
    Graph graph{};
    engine::tools::add_dependency(&graph, 1ULL, 2ULL);
    engine::tools::add_dependency(&graph, 3ULL, 2ULL);

    engine::tools::remove_asset(&graph, 2ULL);

    AssetId deps[8] = {};
    std::size_t count = engine::tools::get_dependencies(&graph, 1ULL, deps, 8);
    if (count != 0U) {
      return 80;
    }

    count = engine::tools::get_dependencies(&graph, 3ULL, deps, 8);
    if (count != 0U) {
      return 81;
    }
  }

  // --- Test 10: JSON serialization round-trip ---
  {
    Graph graph{};
    engine::tools::register_asset_path(&graph, 100ULL, "meshes/hero.mesh");
    engine::tools::register_asset_path(&graph, 200ULL,
                                       "textures/hero_diffuse.png");
    engine::tools::register_asset_path(&graph, 300ULL, "materials/hero_mat");
    engine::tools::add_dependency(&graph, 100ULL, 300ULL);
    engine::tools::add_dependency(&graph, 300ULL, 200ULL);

    char tempPath[512] = {};
    if (!make_temp_graph_path(tempPath, sizeof(tempPath))) {
      return 90;
    }

    if (!engine::tools::write_dependency_graph_json(&graph, tempPath)) {
      return 91;
    }

    Graph loaded{};
    if (!engine::tools::read_dependency_graph_json(&loaded, tempPath)) {
      return 92;
    }

    // Verify edges.
    AssetId deps[8] = {};
    std::size_t count =
        engine::tools::get_dependencies(&loaded, 100ULL, deps, 8);
    if (count != 1U) {
      return 93;
    }
    if (deps[0] != 300ULL) {
      return 94;
    }

    count = engine::tools::get_dependencies(&loaded, 300ULL, deps, 8);
    if (count != 1U) {
      return 95;
    }
    if (deps[0] != 200ULL) {
      return 96;
    }

    // Verify paths.
    auto it = loaded.assetPaths.find(100ULL);
    if ((it == loaded.assetPaths.end()) || (it->second != "meshes/hero.mesh")) {
      return 97;
    }

    // Clean up.
    std::remove(tempPath);
  }

  // --- Test 11: compute_invalidation_set ---
  {
    Graph graph{};
    // mesh1 -> material -> texture
    // mesh2 -> material
    engine::tools::add_dependency(&graph, 10ULL, 30ULL); // mesh1 -> mat
    engine::tools::add_dependency(&graph, 20ULL, 30ULL); // mesh2 -> mat
    engine::tools::add_dependency(&graph, 30ULL, 40ULL); // mat -> texture

    // If texture (40) changes, invalidation set should be {mat(30), mesh1(10),
    // mesh2(20)}.
    AssetId changed[] = {40ULL};
    AssetId invalidated[16] = {};
    const std::size_t count = engine::tools::compute_invalidation_set(
        &graph, changed, 1U, invalidated, 16);
    if (count != 3U) {
      return 100;
    }

    bool found10 = false;
    bool found20 = false;
    bool found30 = false;
    for (std::size_t i = 0U; i < count; ++i) {
      if (invalidated[i] == 10ULL) {
        found10 = true;
      }
      if (invalidated[i] == 20ULL) {
        found20 = true;
      }
      if (invalidated[i] == 30ULL) {
        found30 = true;
      }
    }
    if (!found10 || !found20 || !found30) {
      return 101;
    }
  }

  // --- Test 12: Null/invalid inputs ---
  {
    if (engine::tools::add_dependency(nullptr, 1ULL, 2ULL)) {
      return 110;
    }

    Graph graph{};
    if (engine::tools::add_dependency(&graph, 0ULL, 2ULL)) {
      return 111;
    }
    if (engine::tools::add_dependency(&graph, 1ULL, 0ULL)) {
      return 112;
    }
    if (engine::tools::get_dependencies(nullptr, 1ULL, nullptr, 0) != 0U) {
      return 113;
    }
    if (engine::tools::has_cycle(nullptr)) {
      return 114;
    }
    if (engine::tools::topological_sort(nullptr, nullptr, 0) != 0U) {
      return 115;
    }
    if (engine::tools::write_dependency_graph_json(nullptr, nullptr)) {
      return 116;
    }
    if (engine::tools::read_dependency_graph_json(nullptr, nullptr)) {
      return 117;
    }
  }

  // --- Test 13: Empty graph operations ---
  {
    Graph graph{};
    AssetId deps[8] = {};
    if (engine::tools::get_dependencies(&graph, 1ULL, deps, 8) != 0U) {
      return 120;
    }
    if (engine::tools::get_dependents(&graph, 1ULL, deps, 8) != 0U) {
      return 121;
    }
    if (engine::tools::has_cycle(&graph)) {
      return 122;
    }

    // Topological sort of empty graph returns 0.
    AssetId sorted[8] = {};
    if (engine::tools::topological_sort(&graph, sorted, 8) != 0U) {
      return 123;
    }
  }

  // --- Test 14: Diamond dependency (A->B, A->C, B->D, C->D) ---
  {
    Graph graph{};
    engine::tools::add_dependency(&graph, 1ULL, 2ULL); // A->B
    engine::tools::add_dependency(&graph, 1ULL, 3ULL); // A->C
    engine::tools::add_dependency(&graph, 2ULL, 4ULL); // B->D
    engine::tools::add_dependency(&graph, 3ULL, 4ULL); // C->D

    // If D changes, all of {B, C, A} should be invalidated.
    AssetId changed[] = {4ULL};
    AssetId invalidated[16] = {};
    const std::size_t count = engine::tools::compute_invalidation_set(
        &graph, changed, 1U, invalidated, 16);
    if (count != 3U) {
      return 130;
    }

    // Topo sort should work (no cycle).
    AssetId sorted[8] = {};
    const std::size_t sortedCount =
        engine::tools::topological_sort(&graph, sorted, 8);
    if (sortedCount != 4U) {
      return 131;
    }

    // D must appear before B and C, which must appear before A.
    std::size_t posD = 999U;
    std::size_t posA = 999U;
    for (std::size_t i = 0U; i < sortedCount; ++i) {
      if (sorted[i] == 4ULL) {
        posD = i;
      }
      if (sorted[i] == 1ULL) {
        posA = i;
      }
    }
    if (posD >= posA) {
      return 132;
    }
  }

  std::printf("dependency_graph_test: all tests passed\n");
  return 0;
}
