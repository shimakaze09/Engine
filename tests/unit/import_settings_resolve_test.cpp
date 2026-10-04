// Verifies which import settings an asset gets, on a scratch project:
// - a folder's block reaches the assets below it, however deep;
// - the nearest folder with a block for the asset's type wins, whole;
// - the asset's own block overrides every folder's, whole;
// - a type no folder sets gets its defaults;
// - folders at or above the project root never apply;
// - a malformed sidecar on the way fails the resolution at the defaults;
// - the write time watched for reloads follows every sidecar consulted,
//   and import_settings_write_time every one that could matter;
// - a folder that gains or loses a project document is re-examined, though
//   the answer for an unchanged folder is remembered.

#include "engine/content/import_settings_resolve.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "../test_harness.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"

namespace {

namespace ct = engine::content;

engine::tests::TestContext g_tests;

constexpr const char *kBase = "import_settings_resolve_test_root";

std::string base_path(const char *relative) {
  return std::string(kBase) + "/" + relative;
}

bool write_text(const std::string &path, const char *text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  file.close();
  return !file.fail();
}

/// Gives the folder at `relative` a sidecar with `sidecar`'s blocks.
bool author_folder(const char *relative, ct::AssetSidecar sidecar,
                   const char *guid) {
  sidecar.folder = true;
  return ct::parse_asset_guid(guid, &sidecar.guid) &&
         ct::write_asset_sidecar(base_path(relative).c_str(), sidecar);
}

ct::ResolvedImportSettings resolve(const char *relative, bool *ok) {
  ct::ResolvedImportSettings out{};
  *ok = ct::resolve_import_settings(base_path(relative).c_str(), &out);
  return out;
}

void check_inheritance() {
  // base/project.meta sits beside the project root: never applied.
  ct::AssetSidecar outside{};
  outside.hasTextureImport = true;
  outside.textureImport.wrap = ct::TextureWrapSetting::Clamp;
  g_tests.check(author_folder("project", outside,
                              "10000000-0000-4000-8000-000000000001"),
                "a folder sidecar beside the project root");

  ct::AssetSidecar art{};
  art.hasTextureImport = true;
  art.textureImport.filter = ct::TextureFilterSetting::Nearest;
  art.hasAudioImport = true;
  art.audioImport.sampleRate = 22050U;
  g_tests.check(author_folder("project/assets/art", art,
                              "10000000-0000-4000-8000-000000000002"),
                "art sets textures and sounds");

  bool ok = false;
  const ct::ResolvedImportSettings deep =
      resolve("project/assets/art/ui/icons/play.png", &ok);
  g_tests.check(ok && (deep.kind == ct::ImportSettingsKind::Texture) &&
                    (deep.origin == ct::ImportSettingsOrigin::Folder) &&
                    (deep.texture == art.textureImport) &&
                    (std::strcmp(deep.folder,
                                 base_path("project/assets/art").c_str()) ==
                     0),
                "a texture two folders down gets art's texture block");
  const ct::ResolvedImportSettings sound =
      resolve("project/assets/art/ui/click.wav", &ok);
  g_tests.check(ok && (sound.origin == ct::ImportSettingsOrigin::Folder) &&
                    (sound.audio == art.audioImport),
                "a sound gets the same folder's audio block");
  const ct::ResolvedImportSettings mesh =
      resolve("project/assets/art/ui/frame.gltf", &ok);
  g_tests.check(ok && (mesh.kind == ct::ImportSettingsKind::Mesh) &&
                    (mesh.origin == ct::ImportSettingsOrigin::Defaults) &&
                    (mesh.mesh == ct::MeshImportSettings{}),
                "a mesh, which no folder sets, gets the defaults");

  // ui sets textures too: nearest wins, whole, so art's Nearest filter is
  // not merged into it.
  ct::AssetSidecar ui{};
  ui.hasTextureImport = true;
  ui.textureImport.generateMips = false;
  g_tests.check(author_folder("project/assets/art/ui", ui,
                              "10000000-0000-4000-8000-000000000003"),
                "ui sets textures");
  const ct::ResolvedImportSettings nearer =
      resolve("project/assets/art/ui/icons/play.png", &ok);
  g_tests.check(ok && (nearer.origin == ct::ImportSettingsOrigin::Folder) &&
                    (nearer.texture == ui.textureImport) &&
                    (nearer.texture.filter ==
                     ct::TextureFilterSetting::Linear) &&
                    (std::strcmp(nearer.folder,
                                 base_path("project/assets/art/ui").c_str()) ==
                     0),
                "the nearest folder's block applies whole");
  const ct::ResolvedImportSettings stillArt =
      resolve("project/assets/art/ui/click.wav", &ok);
  g_tests.check(ok && (stillArt.audio == art.audioImport),
                "a type the nearer folder does not set comes from further "
                "up");

  // The asset's own block overrides both folders.
  std::error_code ec{};
  std::filesystem::create_directories(base_path("project/assets/art/ui/icons"),
                                      ec);
  const std::string play = base_path("project/assets/art/ui/icons/play.png");
  ct::AssetSidecar own{};
  own.hasTextureImport = true;
  own.textureImport.colorSpace = ct::TextureColorSpaceSetting::Linear;
  g_tests.check(write_text(play, "x") &&
                    ct::parse_asset_guid("10000000-0000-4000-8000-000000000004",
                                         &own.guid) &&
                    ct::write_asset_sidecar(play.c_str(), own),
                "play.png carries its own block");
  const ct::ResolvedImportSettings asset =
      resolve("project/assets/art/ui/icons/play.png", &ok);
  g_tests.check(ok && (asset.origin == ct::ImportSettingsOrigin::Asset) &&
                    (asset.texture == own.textureImport) &&
                    (asset.folder[0] == '\0'),
                "the asset's own block overrides every folder's");

  // An asset with an identity but no block still inherits.
  const std::string stop = base_path("project/assets/art/ui/icons/stop.png");
  ct::AssetSidecar bare{};
  g_tests.check(write_text(stop, "x") &&
                    ct::parse_asset_guid("10000000-0000-4000-8000-000000000005",
                                         &bare.guid) &&
                    ct::write_asset_sidecar(stop.c_str(), bare),
                "stop.png has an identity and no settings");
  const ct::ResolvedImportSettings inherited =
      resolve("project/assets/art/ui/icons/stop.png", &ok);
  g_tests.check(ok && (inherited.origin == ct::ImportSettingsOrigin::Folder) &&
                    (inherited.texture == ui.textureImport),
                "an asset with no block of its own inherits");

  // Directly under the project root: project.meta, outside it, does not
  // apply, and neither would anything further up.
  const ct::ResolvedImportSettings top = resolve("project/top.png", &ok);
  g_tests.check(ok && (top.origin == ct::ImportSettingsOrigin::Defaults),
                "a folder at or above the project root never applies");
  const ct::ResolvedImportSettings script =
      resolve("project/assets/art/ui/menu.lua", &ok);
  g_tests.check(ok && (script.kind == ct::ImportSettingsKind::None) &&
                    (script.origin == ct::ImportSettingsOrigin::Defaults),
                "a type with no settings resolves to nothing");
}

void check_failures_and_write_time() {
  bool ok = false;
  const ct::ResolvedImportSettings before =
      resolve("project/assets/art/ui/click.wav", &ok);
  const std::int64_t artTime =
      engine::core::file_mtime_ns(base_path("project/assets/art.meta").c_str());
  g_tests.check(ok && (artTime > 0) && (before.newestSidecarWriteTime >= artTime),
                "the write time covers the folder the block came from");

  // A folder in between that will not read stops the walk: the settings
  // below it are unknown, not the defaults.
  g_tests.check(write_text(base_path("project/assets/art/ui.meta"),
                           "{\"schemaVersion\": 1, \"guid\": "
                           "\"10000000-0000-4000-8000-000000000003\", "
                           "\"folder\": true, \"importSettings\": "
                           "{\"sounds\": {}}}"),
                "ui's sidecar is broken");
  const ct::ResolvedImportSettings broken =
      resolve("project/assets/art/ui/click.wav", &ok);
  const std::int64_t uiTime = engine::core::file_mtime_ns(
      base_path("project/assets/art/ui.meta").c_str());
  g_tests.check(!ok && (broken.origin == ct::ImportSettingsOrigin::Defaults) &&
                    (broken.audio == ct::AudioImportSettings{}) &&
                    (broken.newestSidecarWriteTime >= uiTime) &&
                    (std::strcmp(broken.unreadable,
                                 base_path("project/assets/art/ui").c_str()) ==
                     0),
                "a malformed folder sidecar fails at the defaults, named, "
                "and its write time is still watched");
}

void check_watched_write_time() {
  // stop.png's settings come from ui, but a block appearing in art, or its
  // own, would change them, so every sidecar up to the root is watched.
  const std::string stop = base_path("project/assets/art/ui/icons/stop.png");
  const std::string art = base_path("project/assets/art.meta");
  std::error_code ec{};
  std::filesystem::last_write_time(
      art, std::filesystem::file_time_type::clock::now() + std::chrono::hours(1),
      ec);
  const std::int64_t artTime = engine::core::file_mtime_ns(art.c_str());
  g_tests.check(!ec && (artTime > 0) &&
                    (ct::import_settings_write_time(stop.c_str()) == artTime),
                "an edit to a folder further up than the block is watched");

  const std::string icons = base_path("project/assets/art/ui/icons.meta");
  g_tests.check(write_text(icons, "{}"), "icons gains a sidecar");
  std::filesystem::last_write_time(
      icons,
      std::filesystem::file_time_type::clock::now() + std::chrono::hours(2),
      ec);
  const std::int64_t iconsTime = engine::core::file_mtime_ns(icons.c_str());
  g_tests.check(!ec && (iconsTime > artTime) &&
                    (ct::import_settings_write_time(stop.c_str()) == iconsTime),
                "a folder sidecar that appears is watched");
  g_tests.check(ct::import_settings_write_time(
                    base_path("project/top.png").c_str()) == 0,
                "nothing below the project root to watch reads 0");
}

/// Moves `directory`'s write time to `hours` from now: creating or deleting
/// a file moves it too, but the filesystem clock may be too coarse for two
/// steps of one test to land on different ticks.
bool touch_directory(const std::string &directory, int hours) {
  std::error_code ec{};
  std::filesystem::last_write_time(
      directory,
      std::filesystem::file_time_type::clock::now() + std::chrono::hours(hours),
      ec);
  return !ec;
}

void check_project_root_change() {
  std::error_code ec{};
  std::filesystem::create_directories(base_path("project/assets/nest/inner"),
                                      ec);
  ct::AssetSidecar inner{};
  inner.hasTextureImport = true;
  inner.textureImport.filter = ct::TextureFilterSetting::Nearest;
  g_tests.check(author_folder("project/assets/nest/inner", inner,
                              "10000000-0000-4000-8000-000000000009"),
                "nest/inner gets a texture block");
  bool ok = false;
  const ct::ResolvedImportSettings before =
      resolve("project/assets/nest/inner/x.png", &ok);
  g_tests.check(ok && (before.origin == ct::ImportSettingsOrigin::Folder),
                "inner's block applies while inner is a plain folder");

  // inner becomes a project of its own: its sidecar now sits beside a
  // project root and no longer applies.
  const std::string document = base_path("project/assets/nest/inner/i.project");
  g_tests.check(write_text(document, "{}") &&
                    touch_directory(base_path("project/assets/nest/inner"), 3),
                "inner gains a project document");
  const ct::ResolvedImportSettings nested =
      resolve("project/assets/nest/inner/x.png", &ok);
  g_tests.check(ok && (nested.origin == ct::ImportSettingsOrigin::Defaults),
                "a folder that becomes a project root is re-examined");

  std::filesystem::remove(document, ec);
  g_tests.check(!ec &&
                    touch_directory(base_path("project/assets/nest/inner"), 4),
                "inner loses its project document");
  const ct::ResolvedImportSettings after =
      resolve("project/assets/nest/inner/x.png", &ok);
  g_tests.check(ok && (after.origin == ct::ImportSettingsOrigin::Folder),
                "a folder that stops being a project root is re-examined");
}

} // namespace

/// Runs the import settings resolution suite.
int main() {
  static_cast<void>(engine::core::initialize_logging());
  std::error_code ec{};
  std::filesystem::remove_all(kBase, ec);
  std::filesystem::create_directories(base_path("project/assets/art/ui/icons"),
                                      ec);
  if (!write_text(base_path("project/game.project"), "{}")) {
    g_tests.fail("write the project document");
  } else {
    check_inheritance();
    check_failures_and_write_time();
    check_watched_write_time();
    check_project_root_change();
  }
  std::filesystem::remove_all(kBase, ec);
  engine::core::shutdown_logging();
  return g_tests.finish("import settings resolve");
}
