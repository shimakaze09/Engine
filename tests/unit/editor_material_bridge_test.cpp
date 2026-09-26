// Verifies the material editor bridge (issue #160): no published service
// yields found=false everywhere, a load surfaces the resolved state, a
// live param edit is visible immediately (no disk round trip), Save
// persists it, a malformed reload preserves the previous live state, and
// a parent's edits reach its children while a saved child stays an
// instance.

#include "engine/content/asset_catalog.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"

#include "../material_ref_fixture.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr const char *kMountPrefix = "edmat";
constexpr const char *kOsPath = "editor_material_bridge_test.json";
constexpr const char *kVirtualPath = "edmat/editor_material_bridge_test.json";

bool write_file(const char *path, const char *text) noexcept {
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

bool exactly_equal(float lhs, float rhs) noexcept { return lhs == rhs; }

/// Without a published service every bridge entry point fails cleanly.
int check_without_service_fails() noexcept {
  engine::runtime::set_editor_asset_service(nullptr);
  if (engine::runtime::editor_load_material(kVirtualPath).found) {
    return 1;
  }
  if (engine::runtime::editor_reload_material(kVirtualPath).found) {
    return 2;
  }
  if (engine::runtime::editor_save_material(kVirtualPath, nullptr)) {
    return 3;
  }
  engine::renderer::Material params{};
  engine::renderer::MaterialTextureSlots slots{};
  if (engine::runtime::editor_set_material_params(1ULL, params, slots)) {
    return 4;
  }
  return 0;
}

/// Load, live-edit (visible without a disk round trip), save, and a
/// malformed-reload-preserves-previous-state pass, all through the bridge.
int check_load_edit_save_reload() noexcept {
  if (!engine::core::initialize_vfs()) {
    return 10;
  }

  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 11;
  }

  engine::runtime::EngineAssetDatabaseService service{};
  std::unique_ptr<engine::content::AssetCatalog> serviceCatalog(
      new (std::nothrow) engine::content::AssetCatalog());
  service.catalog = serviceCatalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);
  const auto finish = [](int result) noexcept {
    engine::runtime::set_editor_asset_service(nullptr);
    engine::core::shutdown_vfs();
    return result;
  };

  if (!engine::core::mount(kMountPrefix, ".")) {
    return finish(12);
  }

  if (!write_file(kOsPath,
                  "{\"version\":4,\"roughness\":0.4,\"metallic\":0.1}")) {
    remove_file(kOsPath);
    return finish(13);
  }

  const engine::runtime::EditorMaterialState loaded =
      engine::runtime::editor_load_material(kVirtualPath);
  if (!loaded.found || !exactly_equal(loaded.params.roughness, 0.4F) ||
      !exactly_equal(loaded.params.metallic, 0.1F) || loaded.hasParent) {
    remove_file(kOsPath);
    return finish(14);
  }

  // Live edit: mutates the in-memory record directly, visible on the very
  // next query with no disk round trip.
  engine::renderer::Material edited = loaded.params;
  edited.roughness = 0.9F;
  edited.metallic = 0.5F;
  edited.albedo = engine::math::Vec3(0.1F, 0.2F, 0.3F);
  if (!engine::runtime::editor_set_material_params(
          loaded.materialId, edited, loaded.textureSlots)) {
    remove_file(kOsPath);
    return finish(15);
  }

  const engine::runtime::EditorMaterialState afterEdit =
      engine::runtime::editor_load_material(kVirtualPath);
  if (!afterEdit.found || !exactly_equal(afterEdit.params.roughness, 0.9F) ||
      !exactly_equal(afterEdit.params.metallic, 0.5F)) {
    remove_file(kOsPath);
    return finish(16);
  }

  // Save persists the live (edited) state; a subsequent reload from disk
  // must see the saved values, not the original file content.
  if (!engine::runtime::editor_save_material(kVirtualPath, nullptr)) {
    remove_file(kOsPath);
    return finish(17);
  }
  const engine::runtime::EditorMaterialState afterSaveReload =
      engine::runtime::editor_reload_material(kVirtualPath);
  remove_file(kOsPath);
  if (!afterSaveReload.found ||
      !exactly_equal(afterSaveReload.params.roughness, 0.9F) ||
      !exactly_equal(afterSaveReload.params.metallic, 0.5F)) {
    return finish(18);
  }

  // Malformed reload: corrupt the file, reload, expect found=false and the
  // live record (queried via editor_load_material, an already-Ready hit)
  // unchanged from the last successful save.
  if (!write_file(kOsPath, "{ not json")) {
    remove_file(kOsPath);
    return finish(19);
  }
  const engine::runtime::EditorMaterialState badReload =
      engine::runtime::editor_reload_material(kVirtualPath);
  const engine::runtime::EditorMaterialState stillGood =
      engine::runtime::editor_load_material(kVirtualPath);
  remove_file(kOsPath);
  if (badReload.found) {
    return finish(20);
  }
  if (!stillGood.found || !exactly_equal(stillGood.params.roughness, 0.9F) ||
      !exactly_equal(stillGood.params.metallic, 0.5F)) {
    return finish(21);
  }

  return finish(0);
}

/// Reads a whole small file into `out`; false when it cannot.
bool read_file(const char *path, char *out, std::size_t capacity) noexcept {
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
  const std::size_t read = std::fread(out, 1U, capacity - 1U, file);
  std::fclose(file);
  out[read] = '\0';
  return read > 0U;
}

/// A parent's edits reach its children, and a saved child stays an
/// instance of its parent (#543). A child's values were baked from its
/// parent once, at load: neither a live edit nor a reload of the parent
/// reached it, and saving the child wrote every field, so later parent
/// edits could not reach it even after a restart.
int check_parent_changes_reach_child() noexcept {
  constexpr const char *kParentOs = "editor_material_parent_test.json";
  constexpr const char *kParentVirtual =
      "edmat/editor_material_parent_test.json";
  constexpr const char *kChildOs = "editor_material_child_test.json";
  constexpr const char *kChildVirtual = "edmat/editor_material_child_test.json";

  if (!engine::core::initialize_vfs()) {
    return 30;
  }
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 31;
  }
  engine::runtime::EngineAssetDatabaseService service{};
  std::unique_ptr<engine::content::AssetCatalog> serviceCatalog(
      new (std::nothrow) engine::content::AssetCatalog());
  service.catalog = serviceCatalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);
  const auto finish = [&](int result) noexcept {
    remove_file(kParentOs);
    remove_file(kChildOs);
    engine::runtime::set_editor_asset_service(nullptr);
    engine::core::shutdown_vfs();
    return result;
  };
  if (!engine::core::mount(kMountPrefix, ".")) {
    return finish(32);
  }

  const engine::tests::MaterialRefText parentRef =
      engine::tests::catalog_material(service.catalog, kParentVirtual);
  const engine::tests::MaterialRefText childRef =
      engine::tests::catalog_material(service.catalog, kChildVirtual);
  char childJson[160] = {};
  std::snprintf(childJson, sizeof(childJson),
                "{\"version\":4,\"parent\":\"%s\",\"roughness\":0.9}",
                parentRef.text);
  if (!write_file(kParentOs, "{\"version\":4,\"albedo\":[1,0,0],"
                             "\"roughness\":0.2,\"metallic\":0.1}") ||
      !write_file(kChildOs, childJson)) {
    return finish(33);
  }
  const auto child = [&]() noexcept {
    return engine::runtime::editor_load_material(kChildVirtual);
  };
  const engine::runtime::EditorMaterialState loaded = child();
  if (!loaded.found || !loaded.hasParent ||
      !exactly_equal(loaded.params.roughness, 0.9F) ||
      !exactly_equal(loaded.params.metallic, 0.1F)) {
    return finish(34);
  }

  // A live edit of the parent shows on the child at once.
  engine::runtime::EditorMaterialState parent =
      engine::runtime::editor_load_material(kParentVirtual);
  engine::renderer::Material edited = parent.params;
  edited.metallic = 0.6F;
  if (!parent.found || !engine::runtime::editor_set_material_params(
                           parent.materialId, edited, parent.textureSlots)) {
    return finish(35);
  }
  if (!exactly_equal(child().params.metallic, 0.6F) ||
      !exactly_equal(child().params.roughness, 0.9F)) {
    std::printf("live parent edit: child metallic %.2f\n",
                static_cast<double>(child().params.metallic));
    return finish(36);
  }

  // So does a reload of the parent from disk.
  if (!write_file(kParentOs, "{\"version\":4,\"albedo\":[0,0,1],"
                             "\"roughness\":0.2,\"metallic\":0.3}") ||
      !engine::runtime::editor_reload_material(kParentVirtual).found) {
    return finish(37);
  }
  engine::runtime::EditorMaterialState afterReload = child();
  if (!exactly_equal(afterReload.params.metallic, 0.3F) ||
      !exactly_equal(afterReload.params.albedo.z, 1.0F) ||
      !exactly_equal(afterReload.params.albedo.x, 0.0F) ||
      !exactly_equal(afterReload.params.roughness, 0.9F)) {
    std::printf("parent reload: child metallic %.2f albedo.z %.2f\n",
                static_cast<double>(afterReload.params.metallic),
                static_cast<double>(afterReload.params.albedo.z));
    return finish(38);
  }

  // Saving the child writes what it overrides and nothing it inherits.
  char text[1024] = {};
  if (!engine::runtime::editor_save_material(kChildVirtual, kParentVirtual) ||
      !read_file(kChildOs, text, sizeof(text))) {
    return finish(39);
  }
  if ((std::strstr(text, parentRef.text) == nullptr) ||
      (std::strstr(text, "\"roughness\"") == nullptr) ||
      (std::strstr(text, "\"metallic\"") != nullptr) ||
      (std::strstr(text, "\"albedo\"") != nullptr)) {
    std::printf("saved child: %s\n", text);
    return finish(40);
  }

  // An edit on the child is an override: saved, and kept over the parent.
  engine::renderer::Material childEdit = child().params;
  childEdit.metallic = 0.8F;
  if (!engine::runtime::editor_set_material_params(loaded.materialId, childEdit,
                                                   loaded.textureSlots) ||
      !engine::runtime::editor_save_material(kChildVirtual, kParentVirtual) ||
      !read_file(kChildOs, text, sizeof(text)) ||
      (std::strstr(text, "\"metallic\"") == nullptr) ||
      (std::strstr(text, "\"albedo\"") != nullptr)) {
    std::printf("saved child after edit: %s\n", text);
    return finish(41);
  }

  // After a restart's worth of reloads the saved child still follows its
  // parent for everything it does not override.
  if (!write_file(kParentOs, "{\"version\":4,\"albedo\":[0,1,0],"
                             "\"roughness\":0.2,\"metallic\":0.05}") ||
      !engine::runtime::editor_reload_material(kParentVirtual).found ||
      !engine::runtime::editor_reload_material(kChildVirtual).found) {
    return finish(42);
  }
  const engine::runtime::EditorMaterialState reread = child();
  if (!exactly_equal(reread.params.albedo.y, 1.0F) ||
      !exactly_equal(reread.params.albedo.z, 0.0F) ||
      !exactly_equal(reread.params.metallic, 0.8F) ||
      !exactly_equal(reread.params.roughness, 0.9F)) {
    std::printf("reread child: albedo.y %.2f metallic %.2f\n",
                static_cast<double>(reread.params.albedo.y),
                static_cast<double>(reread.params.metallic));
    return finish(43);
  }

  // A parent reload that would make the chain a cycle is refused, and the
  // parent keeps serving what it had.
  char cycleJson[160] = {};
  std::snprintf(cycleJson, sizeof(cycleJson),
                "{\"version\":4,\"parent\":\"%s\",\"metallic\":0.7}",
                childRef.text);
  if (!write_file(kParentOs, cycleJson)) {
    return finish(44);
  }
  if (engine::runtime::editor_reload_material(kParentVirtual).found) {
    return finish(45);
  }
  parent = engine::runtime::editor_load_material(kParentVirtual);
  if (!parent.found || parent.hasParent ||
      !exactly_equal(parent.params.metallic, 0.05F)) {
    return finish(46);
  }
  return finish(0);
}

/// A child that clears a texture slot its parent fills keeps it cleared
/// across a save and a reload (#665). The writer could only omit the slot,
/// and an omitted slot inherits, so the parent's texture came back on the
/// next load with no diagnostic.
int check_child_clears_inherited_texture() noexcept {
  constexpr const char *kParentOs = "editor_material_clear_parent.json";
  constexpr const char *kParentVirtual =
      "edmat/editor_material_clear_parent.json";
  constexpr const char *kChildOs = "editor_material_clear_child.json";
  constexpr const char *kChildVirtual =
      "edmat/editor_material_clear_child.json";

  if (!engine::core::initialize_vfs()) {
    return 60;
  }
  std::unique_ptr<engine::renderer::AssetDatabase> database(
      new (std::nothrow) engine::renderer::AssetDatabase());
  if (database == nullptr) {
    engine::core::shutdown_vfs();
    return 61;
  }
  engine::runtime::EngineAssetDatabaseService service{};
  std::unique_ptr<engine::content::AssetCatalog> serviceCatalog(
      new (std::nothrow) engine::content::AssetCatalog());
  service.catalog = serviceCatalog.get();
  service.database = database.get();
  engine::runtime::set_editor_asset_service(&service);
  const auto finish = [&](int result) noexcept {
    remove_file(kParentOs);
    remove_file(kChildOs);
    engine::runtime::set_editor_asset_service(nullptr);
    engine::core::shutdown_vfs();
    return result;
  };
  if (!engine::core::mount(kMountPrefix, ".")) {
    return finish(62);
  }
  char parentJson[160] = {};
  char childJson[128] = {};
  std::snprintf(parentJson, sizeof(parentJson),
                "{\"version\":4,\"roughness\":0.5,\"textures\":"
                "{\"albedo\":\"%s\"}}",
                engine::tests::catalog_texture(service.catalog,
                                               "edmat/clear_parent_albedo.png")
                    .text);
  std::snprintf(
      childJson, sizeof(childJson), "{\"version\":4,\"parent\":\"%s\"}",
      engine::tests::catalog_material(service.catalog, kParentVirtual).text);
  if (!write_file(kParentOs, parentJson) || !write_file(kChildOs, childJson)) {
    return finish(63);
  }

  const engine::runtime::EditorMaterialState loaded =
      engine::runtime::editor_load_material(kChildVirtual);
  if (!loaded.found || !loaded.hasParent ||
      (loaded.textureSlots.albedo == engine::content::kInvalidAssetId)) {
    return finish(64); // the child starts out inheriting the albedo
  }

  // Clear the inherited slot, save, and reload from disk.
  engine::renderer::MaterialTextureSlots cleared = loaded.textureSlots;
  cleared.albedo = engine::content::kInvalidAssetId;
  if (!engine::runtime::editor_set_material_params(loaded.materialId,
                                                   loaded.params, cleared) ||
      !engine::runtime::editor_save_material(kChildVirtual, kParentVirtual)) {
    return finish(65);
  }
  const engine::runtime::EditorMaterialState reloaded =
      engine::runtime::editor_reload_material(kChildVirtual);
  if (!reloaded.found) {
    return finish(66);
  }
  if (reloaded.textureSlots.albedo != engine::content::kInvalidAssetId) {
    std::printf("cleared slot came back from the parent\n");
    return finish(67);
  }

  // A root material has nothing to clear: null there is refused rather
  // than read as a second spelling of an absent slot.
  if (!write_file(kParentOs, "{\"version\":4,\"roughness\":0.5,"
                             "\"textures\":{\"albedo\":null}}") ||
      engine::runtime::editor_reload_material(kParentVirtual).found) {
    return finish(68);
  }
  return finish(0);
}

} // namespace

int main() {
  int result = check_without_service_fails();
  if (result != 0) {
    std::fprintf(stderr, "editor_material_bridge_test failed: %d\n", result);
    return result;
  }

  result = check_load_edit_save_reload();
  if (result != 0) {
    std::fprintf(stderr, "editor_material_bridge_test failed: %d\n", result);
    return result;
  }

  result = check_parent_changes_reach_child();
  if (result != 0) {
    std::fprintf(stderr, "editor_material_bridge_test failed: %d\n", result);
    return result;
  }

  result = check_child_clears_inherited_texture();
  if (result != 0) {
    std::fprintf(stderr, "editor_material_bridge_test failed: %d\n", result);
    return result;
  }

  std::printf("editor_material_bridge_test: all tests passed\n");
  return 0;
}
