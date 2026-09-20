// Verifies asset hash test behavior for the Engine test suite.

#include "engine/content/asset_metadata.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

static int test_100k_hash_no_collisions() {
  constexpr std::size_t kPathCount = 100000U;
  std::set<engine::content::AssetId> ids{};
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

    const engine::content::AssetId id =
        engine::content::make_asset_id_from_path(path);
    if (id == engine::content::kInvalidAssetId) {
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
  const engine::content::AssetId id1 =
      engine::content::make_asset_id_from_path(path);
  const engine::content::AssetId id2 =
      engine::content::make_asset_id_from_path(path);
  if (id1 != id2) {
    std::fprintf(stderr, "FAIL: same path produced different ids\n");
    return 1;
  }

  std::printf("PASS: deterministic hash\n");
  return 0;
}

static int test_invalid_path_returns_invalid_id() {
  const engine::content::AssetId id =
      engine::content::make_asset_id_from_path(nullptr);
  if (id != engine::content::kInvalidAssetId) {
    std::fprintf(stderr, "FAIL: nullptr path should return kInvalidAssetId\n");
    return 1;
  }

  const engine::content::AssetId idEmpty =
      engine::content::make_asset_id_from_path("");
  if (idEmpty == engine::content::kInvalidAssetId) {
    std::fprintf(stderr,
                 "FAIL: empty string should still produce a valid hash\n");
    return 1;
  }

  std::printf("PASS: invalid path handling\n");
  return 0;
}

/// One asset has one id however its path was spelled: the id is derived
/// from the canonical virtual path, so a doubled separator, a backslash
/// or a trailing slash cannot split one asset into two identities that a
/// saved reference then fails to resolve.
static int test_spelling_does_not_change_identity() {
  const engine::content::AssetId canonical =
      engine::content::make_asset_id_from_path("assets/props/coin.mesh");
  const char *sameAsset[] = {
      "assets//props/coin.mesh",
      "assets///props////coin.mesh",
      "assets\\props\\coin.mesh",
      "assets\\\\props//coin.mesh",
      "assets/props/coin.mesh/",
  };
  for (const char *spelling : sameAsset) {
    const engine::content::AssetId id =
        engine::content::make_asset_id_from_path(spelling);
    if (id != canonical) {
      std::fprintf(stderr,
                   "FAIL: '%s' owns id %016llX, not the canonical %016llX\n",
                   spelling, static_cast<unsigned long long>(id),
                   static_cast<unsigned long long>(canonical));
      return 1;
    }
  }

  // A scheme is not a doubled separator: the built-in primitives keep
  // their own identities.
  const engine::content::AssetId builtinCube =
      engine::content::make_asset_id_from_path("builtin://cube");
  if (builtinCube ==
      engine::content::make_asset_id_from_path("builtin:/cube")) {
    std::fprintf(stderr, "FAIL: a scheme's // was collapsed\n");
    return 1;
  }
  if (builtinCube !=
      engine::content::make_asset_id_from_path("builtin://cube/")) {
    std::fprintf(stderr, "FAIL: a trailing slash changed a builtin id\n");
    return 1;
  }

  // Distinct assets stay distinct.
  if (canonical ==
      engine::content::make_asset_id_from_path("assets/props/gem.mesh")) {
    std::fprintf(stderr, "FAIL: two assets share one id\n");
    return 1;
  }

  // A path too long to be an identity is refused rather than truncated.
  const std::string tooLong(600U, 'a');
  if (engine::content::make_asset_id_from_path(tooLong.c_str()) !=
      engine::content::kInvalidAssetId) {
    std::fprintf(stderr, "FAIL: an overlong path produced an id\n");
    return 1;
  }

  std::printf("PASS: spelling does not change identity\n");
  return 0;
}

/// Runs this executable or test program.
int main() {
  int failures = 0;
  failures += test_100k_hash_no_collisions();
  failures += test_deterministic_hash();
  failures += test_invalid_path_returns_invalid_id();
  failures += test_spelling_does_not_change_identity();
  return (failures == 0) ? 0 : 1;
}
