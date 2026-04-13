#include "engine/renderer/asset_database.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

static int test_100k_hash_no_collisions() {
  constexpr std::size_t kPathCount = 100000U;
  std::set<engine::renderer::AssetId> ids{};
  std::size_t collisions = 0U;

  for (std::size_t i = 0U; i < kPathCount; ++i) {
    // Generate paths like: "assets/meshes/model_00000.mesh" ...
    // "assets/textures/tex_99999.png"
    char path[256] = {};
    if (i < kPathCount / 3U) {
      std::snprintf(path, sizeof(path), "assets/meshes/model_%05zu.mesh", i);
    } else if (i < (kPathCount * 2U) / 3U) {
      std::snprintf(path, sizeof(path), "assets/textures/tex_%05zu.png",
                    i - kPathCount / 3U);
    } else {
      std::snprintf(path, sizeof(path), "assets/scripts/script_%05zu.lua",
                    i - (kPathCount * 2U) / 3U);
    }

    const engine::renderer::AssetId id =
        engine::renderer::make_asset_id_from_path(path);
    if (id == engine::renderer::kInvalidAssetId) {
      std::fprintf(stderr, "FAIL: null path produced invalid id at i=%zu\n", i);
      return 1;
    }

    auto result = ids.insert(id);
    if (!result.second) {
      ++collisions;
      std::fprintf(stderr, "COLLISION at i=%zu path=%s id=%llu\n", i, path,
                   static_cast<unsigned long long>(id));
    }
  }

  if (collisions > 0U) {
    std::fprintf(stderr, "FAIL: %zu collisions in %zu paths\n", collisions,
                 kPathCount);
    return 1;
  }

  std::printf("PASS: 100K paths hashed with zero collisions\n");
  return 0;
}

static int test_deterministic_hash() {
  const char *path = "assets/meshes/test_model.mesh";
  const engine::renderer::AssetId id1 =
      engine::renderer::make_asset_id_from_path(path);
  const engine::renderer::AssetId id2 =
      engine::renderer::make_asset_id_from_path(path);
  if (id1 != id2) {
    std::fprintf(stderr, "FAIL: same path produced different ids\n");
    return 1;
  }

  std::printf("PASS: deterministic hash\n");
  return 0;
}

static int test_invalid_path_returns_invalid_id() {
  const engine::renderer::AssetId id =
      engine::renderer::make_asset_id_from_path(nullptr);
  if (id != engine::renderer::kInvalidAssetId) {
    std::fprintf(stderr, "FAIL: nullptr path should return kInvalidAssetId\n");
    return 1;
  }

  const engine::renderer::AssetId idEmpty =
      engine::renderer::make_asset_id_from_path("");
  if (idEmpty == engine::renderer::kInvalidAssetId) {
    std::fprintf(stderr,
                 "FAIL: empty string should still produce a valid hash\n");
    return 1;
  }

  std::printf("PASS: invalid path handling\n");
  return 0;
}

int main() {
  int failures = 0;
  failures += test_100k_hash_no_collisions();
  failures += test_deterministic_hash();
  failures += test_invalid_path_returns_invalid_id();
  return (failures == 0) ? 0 : 1;
}
