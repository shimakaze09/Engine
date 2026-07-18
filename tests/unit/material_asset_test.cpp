// Verifies material asset loading, instance (parent-chain) resolution, and
// asset database registration for the Engine test suite.

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

namespace {

/// Exact float comparison: every tested value is exactly representable and
/// never crosses lossy text formatting wider than round-trip precision.
bool nearly_equal(float lhs, float rhs) noexcept {
  const float diff = (lhs > rhs) ? (lhs - rhs) : (rhs - lhs);
  return diff <= 0.0001F;
}

/// Writes a material JSON file into the mounted test directory.
bool write_material_file(const char *path, const char *text) noexcept {
  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t size = std::strlen(text);
  const std::size_t written = std::fwrite(text, 1U, size, file);
  std::fclose(file);
  return written == size;
}

void remove_file(const char *path) noexcept {
  static_cast<void>(std::remove(path));
}

/// Full material file: every field present must land exactly.
int verify_full_material_load(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_test_full.json";
  constexpr const char *virtualPath = "mat/material_test_full.json";
  constexpr const char *kJson =
      "{\"version\":1,\"albedo\":[0.25,0.5,0.75],"
      "\"emissive\":[0.125,0.0,1.0],"
      "\"roughness\":0.25,\"metallic\":1.0,\"opacity\":0.5}";
  if (!write_material_file(kPath, kJson)) {
    return 10;
  }

  engine::renderer::AssetId id = engine::renderer::kInvalidAssetId;
  const bool loaded =
      engine::renderer::load_material_asset(database, virtualPath, &id);
  remove_file(kPath);
  if (!loaded || (id == engine::renderer::kInvalidAssetId)) {
    return 11;
  }

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if (params == nullptr) {
    return 12;
  }
  if (!nearly_equal(params->albedo.x, 0.25F) ||
      !nearly_equal(params->albedo.y, 0.5F) ||
      !nearly_equal(params->albedo.z, 0.75F) ||
      !nearly_equal(params->emissive.x, 0.125F) ||
      !nearly_equal(params->emissive.y, 0.0F) ||
      !nearly_equal(params->emissive.z, 1.0F) ||
      !nearly_equal(params->roughness, 0.25F) ||
      !nearly_equal(params->metallic, 1.0F) ||
      !nearly_equal(params->opacity, 0.5F)) {
    return 13;
  }

  if (engine::renderer::material_asset_state(database, id) !=
      engine::renderer::AssetState::Ready) {
    return 14;
  }

  const engine::renderer::AssetMetadata *metadata =
      engine::renderer::find_asset_metadata(database, id);
  if ((metadata == nullptr) ||
      (metadata->typeTag != engine::renderer::AssetTypeTag::Material)) {
    return 15;
  }

  return 0;
}

/// Partial file: unspecified fields must keep Material defaults exactly.
int verify_partial_material_defaults(
    engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_test_partial.json";
  constexpr const char *virtualPath = "mat/material_test_partial.json";
  if (!write_material_file(kPath, "{\"roughness\":0.75}")) {
    return 20;
  }

  engine::renderer::AssetId id = engine::renderer::kInvalidAssetId;
  const bool loaded =
      engine::renderer::load_material_asset(database, virtualPath, &id);
  remove_file(kPath);
  if (!loaded) {
    return 21;
  }

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if (params == nullptr) {
    return 22;
  }

  const engine::renderer::Material defaults{};
  if (!nearly_equal(params->roughness, 0.75F) ||
      !nearly_equal(params->albedo.x, defaults.albedo.x) ||
      !nearly_equal(params->albedo.y, defaults.albedo.y) ||
      !nearly_equal(params->albedo.z, defaults.albedo.z) ||
      !nearly_equal(params->metallic, defaults.metallic) ||
      !nearly_equal(params->opacity, defaults.opacity)) {
    return 23;
  }

  return 0;
}

/// Instance chain: the child starts from the parent's resolved values and
/// overrides only what it specifies; a grandchild sees both layers.
int verify_parent_chain_resolution(engine::renderer::AssetDatabase *database) {
  constexpr const char *kBasePath = "material_test_base.json";
  constexpr const char *kChildPath = "material_test_child.json";
  constexpr const char *kGrandPath = "material_test_grand.json";

  if (!write_material_file(
          kBasePath,
          "{\"albedo\":[1.0,0.0,0.0],\"roughness\":0.125,\"metallic\":0.5}") ||
      !write_material_file(
          kChildPath,
          "{\"parent\":\"mat/material_test_base.json\",\"metallic\":1.0}") ||
      !write_material_file(
          kGrandPath,
          "{\"parent\":\"mat/material_test_child.json\",\"opacity\":0.25}")) {
    remove_file(kBasePath);
    remove_file(kChildPath);
    remove_file(kGrandPath);
    return 30;
  }

  engine::renderer::AssetId grandId = engine::renderer::kInvalidAssetId;
  const bool loaded =
      engine::renderer::load_material_asset(database, "mat/material_test_grand.json", &grandId);
  remove_file(kBasePath);
  remove_file(kChildPath);
  remove_file(kGrandPath);
  if (!loaded) {
    return 31;
  }

  const engine::renderer::Material *grand =
      engine::renderer::find_material_params(database, grandId);
  if (grand == nullptr) {
    return 32;
  }
  // From base: albedo + roughness. From child: metallic. Own: opacity.
  if (!nearly_equal(grand->albedo.x, 1.0F) ||
      !nearly_equal(grand->albedo.y, 0.0F) ||
      !nearly_equal(grand->albedo.z, 0.0F) ||
      !nearly_equal(grand->roughness, 0.125F) ||
      !nearly_equal(grand->metallic, 1.0F) ||
      !nearly_equal(grand->opacity, 0.25F)) {
    return 33;
  }

  // The intermediate materials registered too, with their own values.
  const engine::renderer::AssetId childId =
      engine::renderer::make_asset_id_from_path("mat/material_test_child.json");
  const engine::renderer::Material *child =
      engine::renderer::find_material_params(database, childId);
  if ((child == nullptr) || !nearly_equal(child->metallic, 1.0F) ||
      !nearly_equal(child->opacity, 1.0F)) {
    return 34;
  }

  // The dependency edge child -> base was recorded.
  const engine::renderer::AssetId baseId =
      engine::renderer::make_asset_id_from_path("mat/material_test_base.json");
  engine::renderer::AssetId deps[4] = {};
  const std::size_t depCount =
      engine::renderer::get_dependencies(database, childId, deps, 4U);
  if ((depCount != 1U) || (deps[0] != baseId)) {
    return 35;
  }

  return 0;
}

/// Failure paths: cycles, missing parents, malformed fields, bad versions.
int verify_material_load_failures(engine::renderer::AssetDatabase *database) {
  constexpr const char *kCyclePathA = "material_test_cycle_a.json";
  constexpr const char *kCyclePathB = "material_test_cycle_b.json";
  if (!write_material_file(
          kCyclePathA,
          "{\"parent\":\"mat/material_test_cycle_b.json\"}") ||
      !write_material_file(
          kCyclePathB,
          "{\"parent\":\"mat/material_test_cycle_a.json\"}")) {
    remove_file(kCyclePathA);
    remove_file(kCyclePathB);
    return 40;
  }
  const bool cycleLoaded =
      engine::renderer::load_material_asset(database, "mat/material_test_cycle_a.json", nullptr);
  remove_file(kCyclePathA);
  remove_file(kCyclePathB);
  if (cycleLoaded) {
    return 41;
  }

  constexpr const char *kOrphanPath = "material_test_orphan.json";
  if (!write_material_file(kOrphanPath,
                           "{\"parent\":\"mat/material_test_missing.json\"}")) {
    return 42;
  }
  const bool orphanLoaded =
      engine::renderer::load_material_asset(database, "mat/material_test_orphan.json", nullptr);
  remove_file(kOrphanPath);
  if (orphanLoaded) {
    return 43;
  }

  constexpr const char *kBadFieldPath = "material_test_bad_field.json";
  if (!write_material_file(kBadFieldPath, "{\"roughness\":\"rough\"}")) {
    return 44;
  }
  const bool badFieldLoaded =
      engine::renderer::load_material_asset(database, "mat/material_test_bad_field.json", nullptr);
  remove_file(kBadFieldPath);
  if (badFieldLoaded) {
    return 45;
  }

  constexpr const char *kBadVec3Path = "material_test_bad_vec3.json";
  if (!write_material_file(kBadVec3Path, "{\"albedo\":[1.0,2.0]}")) {
    return 46;
  }
  const bool badVec3Loaded =
      engine::renderer::load_material_asset(database, "mat/material_test_bad_vec3.json", nullptr);
  remove_file(kBadVec3Path);
  if (badVec3Loaded) {
    return 47;
  }

  constexpr const char *kBadVersionPath = "material_test_bad_version.json";
  if (!write_material_file(kBadVersionPath, "{\"version\":2}")) {
    return 48;
  }
  const bool badVersionLoaded = engine::renderer::load_material_asset(
      database, "mat/material_test_bad_version.json", nullptr);
  remove_file(kBadVersionPath);
  if (badVersionLoaded) {
    return 49;
  }

  if (engine::renderer::load_material_asset(database,
                                            "mat/material_test_absent.json",
                                            nullptr)) {
    return 50;
  }

  return 0;
}

/// Unknown ids resolve to nullptr/Unloaded; clear empties the table.
int verify_material_database_edges(engine::renderer::AssetDatabase *database) {
  if (engine::renderer::find_material_params(database, 12345U) != nullptr) {
    return 60;
  }
  if (engine::renderer::material_asset_state(database, 12345U) !=
      engine::renderer::AssetState::Unloaded) {
    return 61;
  }
  if (engine::renderer::find_material_params(nullptr, 12345U) != nullptr) {
    return 62;
  }

  engine::renderer::Material params{};
  params.metallic = 0.5F;
  if (!engine::renderer::register_material_asset(database, 777U, "direct",
                                                 params)) {
    return 63;
  }
  const engine::renderer::Material *found =
      engine::renderer::find_material_params(database, 777U);
  if ((found == nullptr) || !nearly_equal(found->metallic, 0.5F)) {
    return 64;
  }

  engine::renderer::clear_asset_database(database);
  if (engine::renderer::find_material_params(database, 777U) != nullptr) {
    return 65;
  }

  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
  // Test material files are written to the working directory, mounted
  // under the "mat" virtual prefix.
  if (!engine::core::mount("mat", ".")) {
    engine::core::shutdown_vfs();
    return 2;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 3;
  }

  int result = verify_full_material_load(database.get());
  if (result == 0) {
    result = verify_partial_material_defaults(database.get());
  }
  if (result == 0) {
    result = verify_parent_chain_resolution(database.get());
  }
  if (result == 0) {
    result = verify_material_load_failures(database.get());
  }
  if (result == 0) {
    result = verify_material_database_edges(database.get());
  }

  engine::core::shutdown_vfs();
  return result;
}
