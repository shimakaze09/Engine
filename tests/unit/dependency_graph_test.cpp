// Verifies dependency graph test behavior for the Engine test suite.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Include the dependency graph header directly from the tools directory.
// The test links against the dependency_graph.cpp object.
#include "dependency_graph.h"

/// Runs this executable or test program.
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

    AssetId deps[8] = {};
    std::size_t count = engine::tools::get_dependencies(&graph, kMesh, deps, 8);
    if (count != 1U) {
      return 3;
    }
    if (deps[0] != kMaterial) {
      return 4;
    }

    count = engine::tools::get_dependencies(&graph, kMaterial, deps, 8);
    if (count != 1U) {
      return 5;
    }
    if (deps[0] != kTexture) {
      return 6;
    }

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

    AssetId deps[8] = {};
    const std::size_t count =
        engine::tools::get_all_dependents_recursive(&graph, kTexture, deps, 8);
    if (count != 2U) {
      return 20;
    }

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

  // --- Test 10: compute_invalidation_set ---
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

  // --- Test 11: Null/invalid inputs ---
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

  // --- Test 15: Deterministic ordering (L-02, issue #86) ---
  {
    Graph graph{};
    const AssetId insertOrder[] = {90ULL, 50ULL, 70ULL, 10ULL, 30ULL,
                                   80ULL, 20ULL, 60ULL, 40ULL};
    for (const AssetId dep : insertOrder) {
      engine::tools::add_dependency(&graph, 1000ULL, dep);
      engine::tools::add_dependency(&graph, dep, 2000ULL);
    }

    AssetId deps[16] = {};
    const std::size_t depCount =
        engine::tools::get_dependencies(&graph, 1000ULL, deps, 16);
    if (depCount != 9U) {
      return 140;
    }
    for (std::size_t i = 0U; i < depCount; ++i) {
      if (deps[i] != (static_cast<AssetId>(i + 1U) * 10ULL)) {
        return 141;
      }
    }

    const std::size_t dependentCount =
        engine::tools::get_dependents(&graph, 2000ULL, deps, 16);
    if (dependentCount != 9U) {
      return 142;
    }
    for (std::size_t i = 0U; i < dependentCount; ++i) {
      if (deps[i] != (static_cast<AssetId>(i + 1U) * 10ULL)) {
        return 143;
      }
    }

    // Truncation must keep the smallest ids, not an arbitrary subset.
    AssetId truncated[3] = {};
    if (engine::tools::get_dependencies(&graph, 1000ULL, truncated, 3U) !=
        3U) {
      return 144;
    }
    if ((truncated[0] != 10ULL) || (truncated[1] != 20ULL) ||
        (truncated[2] != 30ULL)) {
      return 145;
    }

    const std::size_t recursiveCount =
        engine::tools::get_all_dependents_recursive(&graph, 2000ULL, deps,
                                                    16);
    if (recursiveCount != 10U) {
      return 146;
    }
    for (std::size_t i = 0U; i < 9U; ++i) {
      if (deps[i] != (static_cast<AssetId>(i + 1U) * 10ULL)) {
        return 147;
      }
    }
    if (deps[9] != 1000ULL) {
      return 147;
    }

    AssetId changed[] = {2000ULL};
    const std::size_t invalidatedCount = engine::tools::compute_invalidation_set(
        &graph, changed, 1U, deps, 16);
    if (invalidatedCount != 10U) {
      return 148;
    }
    for (std::size_t i = 0U; i < 9U; ++i) {
      if (deps[i] != (static_cast<AssetId>(i + 1U) * 10ULL)) {
        return 149;
      }
    }
    if (deps[9] != 1000ULL) {
      return 149;
    }

    // Kahn tie-break: every ready node emits smallest id first, so this
    // graph has exactly one valid output sequence.
    AssetId sorted[16] = {};
    const std::size_t sortedCount =
        engine::tools::topological_sort(&graph, sorted, 16);
    if (sortedCount != 11U) {
      return 150;
    }
    if (sorted[0] != 2000ULL) {
      return 151;
    }
    for (std::size_t i = 0U; i < 9U; ++i) {
      if (sorted[i + 1U] != (static_cast<AssetId>(i + 1U) * 10ULL)) {
        return 152;
      }
    }
    if (sorted[10] != 1000ULL) {
      return 153;
    }
  }

  std::printf("dependency_graph_test: all tests passed\n");
  return 0;
}
