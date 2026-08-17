// Verifies the material editor bridge (issue #160): no published service
// yields found=false everywhere, a load surfaces the resolved state, a
// live param edit is visible immediately (no disk round trip), Save
// persists it, and a malformed reload preserves the previous live state.

#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/service_registry.h"

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

  if (!write_file(kOsPath, "{\"version\":2,\"roughness\":0.4,\"metallic\":0.1}")) {
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

  std::printf("editor_material_bridge_test: all tests passed\n");
  return 0;
}
