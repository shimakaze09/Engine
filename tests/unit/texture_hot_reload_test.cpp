// Verifies texture hot reload (issue #681) against the renderer's own
// tables with a recording loader: a reload of a Ready texture swaps the
// handle, releases the previous one and reaches both the material that
// names the texture and the material inheriting it; a reload whose file
// no longer loads keeps the previous handle and releases nothing; a
// Failed texture recovers once its file loads; an id nothing loaded is
// not reloaded; and the poll reloads only textures whose file time moved.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <system_error>

#include "engine/content/asset_catalog.h"
#include "engine/core/vfs.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/material_loader.h"
#include "engine/renderer/texture_hot_reload.h"

#include "../material_ref_fixture.h"

namespace {

using engine::content::AssetId;
using engine::renderer::TextureHandle;
using engine::renderer::TextureReload;

engine::content::AssetCatalog *g_catalog = nullptr;

constexpr const char *kTextureOs = "texture_hot_reload_albedo.png";
constexpr const char *kTexturePath = "mat/texture_hot_reload_albedo.png";
constexpr const char *kOtherOs = "texture_hot_reload_other.png";
constexpr const char *kOtherPath = "mat/texture_hot_reload_other.png";
constexpr const char *kParentOs = "texture_hot_reload_parent.json";
constexpr const char *kParentPath = "mat/texture_hot_reload_parent.json";
constexpr const char *kChildOs = "texture_hot_reload_child.json";
constexpr const char *kChildPath = "mat/texture_hot_reload_child.json";

/// What the recording loader saw.
struct LoaderLog final {
  std::uint32_t nextHandle = 100U;
  std::size_t loads = 0U;
  std::size_t releases = 0U;
  TextureHandle lastReleased{};
};

LoaderLog g_log{};

/// Loads a file whose text starts with "good" as a fresh handle; anything
/// else, or a missing file, fails.
TextureHandle recording_load(const char *virtualPath, void *) noexcept {
  ++g_log.loads;
  void *bytes = nullptr;
  std::size_t size = 0U;
  const bool read = static_cast<bool>(
      engine::core::vfs_read_binary(virtualPath, &bytes, &size));
  const bool good =
      read && (size >= 4U) && (std::memcmp(bytes, "good", 4U) == 0);
  if (read) {
    engine::core::vfs_free(bytes);
  }
  if (!good) {
    return engine::renderer::kInvalidTextureHandle;
  }
  return TextureHandle{g_log.nextHandle++};
}

void recording_release(TextureHandle handle, void *) noexcept {
  ++g_log.releases;
  g_log.lastReleased = handle;
}

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

/// Rewrites `path` and moves its modification time a whole second past
/// the previous one, so the change is visible on a filesystem whose time
/// resolution is coarser than two writes in a row.
bool save_file(const char *path, const char *text) noexcept {
  std::error_code ec{};
  const auto before = std::filesystem::last_write_time(path, ec);
  const bool existed = !ec;
  if (!write_file(path, text)) {
    return false;
  }
  if (existed) {
    std::filesystem::last_write_time(path, before + std::chrono::seconds(1),
                                     ec);
  }
  return !ec;
}

void remove_files() noexcept {
  for (const char *path : {kTextureOs, kOtherOs, kParentOs, kChildOs}) {
    static_cast<void>(std::remove(path));
  }
}

TextureHandle albedo_of(engine::renderer::AssetDatabase *database,
                        AssetId material) noexcept {
  const engine::renderer::Material *params =
      engine::renderer::find_material_params(database, material);
  return (params != nullptr) ? params->albedoTexture
                             : engine::renderer::kInvalidTextureHandle;
}

std::size_t resolve(engine::renderer::AssetDatabase *database) noexcept {
  return engine::renderer::resolve_material_textures(database, g_catalog,
                                                     &recording_load, nullptr);
}

/// Loads a parent naming the texture and a child inheriting it, and
/// resolves their handles. Returns 0 on success.
int load_materials(engine::renderer::AssetDatabase *database,
                   const char *textureText, AssetId *parent,
                   AssetId *child) noexcept {
  const auto textureRef =
      engine::tests::catalog_texture(g_catalog, kTexturePath);
  const auto parentRef =
      engine::tests::catalog_material(g_catalog, kParentPath);
  char parentJson[256] = {};
  char childJson[256] = {};
  std::snprintf(parentJson, sizeof(parentJson),
                "{\"version\":4,\"textures\":{\"albedo\":\"%s\"}}",
                textureRef.text);
  std::snprintf(childJson, sizeof(childJson),
                "{\"version\":4,\"parent\":\"%s\",\"roughness\":0.9}",
                parentRef.text);
  if (!write_file(kTextureOs, textureText) ||
      !write_file(kParentOs, parentJson) || !write_file(kChildOs, childJson)) {
    return 1;
  }
  const auto parentId =
      engine::renderer::load_material_asset(database, g_catalog, kParentPath);
  const auto childId =
      engine::renderer::load_material_asset(database, g_catalog, kChildPath);
  if (!parentId.has_value() || !childId.has_value()) {
    return 2;
  }
  *parent = *parentId;
  *child = *childId;
  static_cast<void>(resolve(database));
  return 0;
}

/// A reload swaps a Ready texture, releases the handle it replaced, and
/// reaches the material naming it and the one inheriting it; a reload
/// that fails keeps the handle and releases nothing.
int check_ready_reload(engine::renderer::AssetDatabase *database) noexcept {
  AssetId parent = 0U;
  AssetId child = 0U;
  if (load_materials(database, "good v1", &parent, &child) != 0) {
    return 10;
  }
  const AssetId texture =
      engine::content::make_asset_id_from_path(kTexturePath);
  const TextureHandle first = albedo_of(database, parent);
  if ((first == engine::renderer::kInvalidTextureHandle) ||
      (albedo_of(database, child) != first)) {
    return 11;
  }

  if (!save_file(kTextureOs, "good v2") ||
      (engine::renderer::reload_texture_asset(
           database, g_catalog, texture, &recording_load, &recording_release,
           nullptr) != TextureReload::Reloaded)) {
    return 12;
  }
  if ((g_log.releases != 1U) || (g_log.lastReleased != first)) {
    return 13;
  }
  // Nothing still names the released handle.
  if ((albedo_of(database, parent) == first) ||
      (albedo_of(database, child) == first)) {
    std::printf("a material kept the released handle\n");
    return 14;
  }
  static_cast<void>(resolve(database));
  const TextureHandle second = albedo_of(database, parent);
  if ((second == engine::renderer::kInvalidTextureHandle) ||
      (second == first) || (albedo_of(database, child) != second)) {
    return 15;
  }

  // A save that does not load: the texture keeps serving `second`.
  const std::size_t releasesBefore = g_log.releases;
  if (!save_file(kTextureOs, "broken") ||
      (engine::renderer::reload_texture_asset(
           database, g_catalog, texture, &recording_load, &recording_release,
           nullptr) != TextureReload::Failed)) {
    return 16;
  }
  static_cast<void>(resolve(database));
  if ((g_log.releases != releasesBefore) ||
      (engine::renderer::texture_asset_state(database, texture) !=
       engine::content::AssetState::Ready) ||
      (engine::renderer::resolve_texture_asset(database, texture) != second) ||
      (albedo_of(database, parent) != second) ||
      (albedo_of(database, child) != second)) {
    std::printf("a failed reload disturbed the serving texture\n");
    return 17;
  }
  return 0;
}

/// A texture that failed to load is loaded again once its file loads, and
/// the materials using it pick it up; the poll finds it by its file time.
int check_failed_recovers(engine::renderer::AssetDatabase *database) noexcept {
  AssetId parent = 0U;
  AssetId child = 0U;
  if (load_materials(database, "broken", &parent, &child) != 0) {
    return 20;
  }
  const AssetId texture =
      engine::content::make_asset_id_from_path(kTexturePath);
  if ((engine::renderer::texture_asset_state(database, texture) !=
       engine::content::AssetState::Failed) ||
      (albedo_of(database, parent) !=
       engine::renderer::kInvalidTextureHandle)) {
    return 21;
  }

  // Nothing changed on disk: the poll loads nothing.
  const std::size_t loadsBefore = g_log.loads;
  for (std::size_t sweep = 0U;
       sweep < engine::renderer::AssetDatabase::kMaxTextureAssets /
                   engine::renderer::kTextureReloadPollSlots;
       ++sweep) {
    if (engine::renderer::poll_texture_changes(
            database, g_catalog, &recording_load, &recording_release,
            nullptr) != 0U) {
      return 22;
    }
  }
  if (g_log.loads != loadsBefore) {
    return 23;
  }

  // The fixed file is found within one sweep of the table.
  if (!save_file(kTextureOs, "good fixed")) {
    return 24;
  }
  std::size_t reloaded = 0U;
  for (std::size_t sweep = 0U;
       sweep < engine::renderer::AssetDatabase::kMaxTextureAssets /
                   engine::renderer::kTextureReloadPollSlots;
       ++sweep) {
    reloaded += engine::renderer::poll_texture_changes(
        database, g_catalog, &recording_load, &recording_release, nullptr);
  }
  if ((reloaded != 1U) || (g_log.loads != loadsBefore + 1U)) {
    std::printf("the poll reloaded %zu textures\n", reloaded);
    return 25;
  }
  static_cast<void>(resolve(database));
  const TextureHandle handle = albedo_of(database, parent);
  if ((handle == engine::renderer::kInvalidTextureHandle) ||
      (albedo_of(database, child) != handle)) {
    std::printf("the materials did not pick up the recovered texture\n");
    return 26;
  }
  return 0;
}

/// An id no material resolved has no record to replace.
int check_not_loaded(engine::renderer::AssetDatabase *database) noexcept {
  if (!write_file(kOtherOs, "good") ||
      engine::tests::catalog_texture(g_catalog, kOtherPath).text[0] == '\0') {
    return 30;
  }
  const std::size_t loadsBefore = g_log.loads;
  if ((engine::renderer::reload_texture_asset(
           database, g_catalog,
           engine::content::make_asset_id_from_path(kOtherPath),
           &recording_load, &recording_release,
           nullptr) != TextureReload::NotLoaded) ||
      (g_log.loads != loadsBefore)) {
    return 31;
  }
  return 0;
}

} // namespace

int main() {
  std::unique_ptr<engine::content::AssetCatalog> catalogOwner(
      new (std::nothrow) engine::content::AssetCatalog());
  if (catalogOwner == nullptr) {
    return 1;
  }
  g_catalog = catalogOwner.get();
  if (!engine::core::initialize_vfs()) {
    return 2;
  }
  if (!engine::core::mount("mat", ".")) {
    engine::core::shutdown_vfs();
    return 3;
  }

  int result = 0;
  using Check = int (*)(engine::renderer::AssetDatabase *) noexcept;
  for (const Check check :
       {Check{&check_ready_reload}, Check{&check_failed_recovers},
        Check{&check_not_loaded}}) {
    std::unique_ptr<engine::renderer::AssetDatabase> database(
        new (std::nothrow) engine::renderer::AssetDatabase());
    if (database == nullptr) {
      result = 4;
      break;
    }
    engine::content::clear_asset_catalog(g_catalog);
    g_log = LoaderLog{};
    result = check(database.get());
    remove_files();
    if (result != 0) {
      break;
    }
  }

  engine::core::shutdown_vfs();
  if (result != 0) {
    std::fprintf(stderr, "texture_hot_reload_test failed: %d\n", result);
    return result;
  }
  std::printf("texture_hot_reload_test: all tests passed\n");
  return 0;
}
