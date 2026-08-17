// Verifies reload_material_asset: a good reload replaces the in-memory
// record in place, and a malformed reload leaves the previous valid state
// completely untouched (editor hot-reload safety contract, issue #160).

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"

namespace {

bool exactly_equal(float lhs, float rhs) noexcept { return lhs == rhs; }

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

/// A successful reload fully replaces the resolved values.
int verify_reload_success(engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_reload_ok.json";
  constexpr const char *kVirtualPath = "mat/material_reload_ok.json";

  if (!write_material_file(kPath, "{\"version\":2,\"roughness\":0.2}")) {
    return 10;
  }
  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
  if (!loadResult.has_value()) {
    remove_file(kPath);
    return 11;
  }
  const engine::renderer::AssetId id = *loadResult;

  if (!write_material_file(
          kPath, "{\"version\":2,\"roughness\":0.8,\"metallic\":1.0,"
                "\"textures\":{\"albedo\":\"assets/textures/new.png\"}}")) {
    remove_file(kPath);
    return 12;
  }
  const auto reloadResult =
      engine::renderer::reload_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (!reloadResult.has_value() || (*reloadResult != id)) {
    return 13;
  }

  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if ((params == nullptr) || !exactly_equal(params->roughness, 0.8F) ||
      !exactly_equal(params->metallic, 1.0F)) {
    return 14;
  }

  const engine::renderer::MaterialTextureSlots *slots =
      engine::renderer::find_material_texture_slots(database, id);
  const engine::renderer::AssetId expectedAlbedo =
      engine::renderer::make_asset_id_from_path("assets/textures/new.png");
  if ((slots == nullptr) || (slots->albedo != expectedAlbedo)) {
    return 15;
  }

  return 0;
}

/// A malformed reload leaves the previous, still-valid record untouched.
int verify_reload_malformed_preserves_previous(
    engine::renderer::AssetDatabase *database) {
  constexpr const char *kPath = "material_reload_bad.json";
  constexpr const char *kVirtualPath = "mat/material_reload_bad.json";

  if (!write_material_file(kPath, "{\"version\":2,\"roughness\":0.33,"
                                  "\"metallic\":0.11}")) {
    return 20;
  }
  const auto loadResult =
      engine::renderer::load_material_asset(database, kVirtualPath);
  if (!loadResult.has_value()) {
    remove_file(kPath);
    return 21;
  }
  const engine::renderer::AssetId id = *loadResult;

  // Corrupt the file on disk (malformed JSON) and attempt a reload.
  if (!write_material_file(kPath, "{ this is not valid json")) {
    remove_file(kPath);
    return 22;
  }
  const auto reloadResult =
      engine::renderer::reload_material_asset(database, kVirtualPath);
  remove_file(kPath);
  if (reloadResult.has_value() ||
      (reloadResult.error() != engine::renderer::MaterialLoadError::Parse)) {
    return 23;
  }

  // Previous valid values are exactly as they were before the bad reload.
  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, id);
  if ((params == nullptr) || !exactly_equal(params->roughness, 0.33F) ||
      !exactly_equal(params->metallic, 0.11F)) {
    return 24;
  }
  if (engine::renderer::material_asset_state(database, id) !=
      engine::renderer::AssetState::Ready) {
    return 25;
  }

  return 0;
}

/// Reloading a material that was never loaded fails cleanly (no state to
/// preserve, but also no spurious registration).
int verify_reload_of_unknown_material(
    engine::renderer::AssetDatabase *database) {
  const auto reloadResult = engine::renderer::reload_material_asset(
      database, "mat/material_never_loaded.json");
  if (reloadResult.has_value()) {
    return 30;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::core::initialize_vfs()) {
    return 1;
  }
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

  int result = verify_reload_success(database.get());
  if (result == 0) {
    result = verify_reload_malformed_preserves_previous(database.get());
  }
  if (result == 0) {
    result = verify_reload_of_unknown_material(database.get());
  }

  engine::core::shutdown_vfs();
  return result;
}
