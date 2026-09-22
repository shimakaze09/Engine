// Verifies the Assets panel's per-frame disk work is gone (#528): the
// import-settings sidecar is read once per selection change (and once
// more after an explicit invalidation), its fields parse exactly, a
// missing sidecar is a cached answer too, and a thumbnail that cannot be
// produced is remembered instead of being opened again on every frame.
//
// Also pins where the panel writes: the authored ".meta" beside the
// source, which is the file the cook reads. Writing the cooked record
// instead would let an author change a setting and watch the next cook
// ignore it.

#include "editor_import_settings.h"
#include "editor_session.h"

#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool write_file(const std::string &path, const char *text) {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const std::size_t len = std::strlen(text);
  const bool ok = std::fwrite(text, 1U, len, file) == len;
  std::fclose(file);
  return ok;
}

} // namespace

/// Runs this executable or test program.
int main() {
  using engine::editor::ImportSettingsDocument;
  const std::string dir = "editor_import_settings_test_dir";
  std::error_code ec{};
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  // A mesh SOURCE, because that is where authored settings live.
  const std::string mesh = dir + "/thing.gltf";
  const std::string other = dir + "/other.gltf";
  CHECK(write_file(mesh, "not a real glTF"), "asset written");
  CHECK(write_file(mesh + ".meta",
                   "{\"schemaVersion\":1,"
                   "\"guid\":\"3d9f0a11-7c62-4b8e-9a05-1f2e3d4c5b6a\","
                   "\"importSettings\":{\"meshIndex\":2,"
                   "\"primitiveIndex\":3,\"scaleFactor\":0.5,\"upAxis\":2,"
                   "\"generateNormals\":true}}"),
        "sidecar written");

  const std::uint64_t reads0 = engine::editor::import_settings_read_count();
  const ImportSettingsDocument *doc =
      engine::editor::import_settings_for_asset(mesh.c_str());
  CHECK(doc != nullptr, "document returned");
  CHECK(engine::editor::import_settings_read_count() == reads0 + 1U,
        "first selection reads the sidecar once");
  if (doc != nullptr) {
    CHECK(doc->state == ImportSettingsDocument::State::Valid, "sidecar valid");
    CHECK(doc->hasSettings, "the source carries settings");
    CHECK((doc->settings.meshIndex == 2) &&
              (doc->settings.primitiveIndex == 3) &&
              (doc->settings.scaleFactor == 0.5F) &&
              (doc->settings.upAxis == 2) && doc->settings.generateNormals,
          "fields parse exactly");
  }

  // The same selection drawn again: no read.
  for (int frame = 0; frame < 5; ++frame) {
    static_cast<void>(engine::editor::import_settings_for_asset(mesh.c_str()));
  }
  CHECK(engine::editor::import_settings_read_count() == reads0 + 1U,
        "redrawing the same selection reads nothing");

  // Another selection, missing sidecar: one read, cached as missing.
  doc = engine::editor::import_settings_for_asset(other.c_str());
  CHECK((doc != nullptr) &&
            (doc->state == ImportSettingsDocument::State::Missing),
        "missing sidecar is reported");
  static_cast<void>(engine::editor::import_settings_for_asset(other.c_str()));
  CHECK(engine::editor::import_settings_read_count() == reads0 + 2U,
        "a missing sidecar is read once per selection too");

  // Back, then invalidated: one read each.
  static_cast<void>(engine::editor::import_settings_for_asset(mesh.c_str()));
  CHECK(engine::editor::import_settings_read_count() == reads0 + 3U,
        "changing selection back reads once");
  engine::editor::invalidate_import_settings_cache();
  static_cast<void>(engine::editor::import_settings_for_asset(mesh.c_str()));
  CHECK(engine::editor::import_settings_read_count() == reads0 + 4U,
        "invalidation forces exactly one re-read");
  CHECK(engine::editor::import_settings_for_asset("") == nullptr,
        "an empty path has no document");

  // The panel's write lands in the authored sidecar, keeps the identity
  // the asset already had, and is what a reader sees next.
  engine::content::AssetSidecar before{};
  CHECK(engine::content::read_asset_sidecar(mesh.c_str(), &before) ==
            engine::content::SidecarReadResult::Ok,
        "the source has an identity before the edit");
  engine::content::MeshImportSettings edited{};
  edited.meshIndex = 7;
  edited.primitiveIndex = 1;
  edited.scaleFactor = 2.5F;
  edited.upAxis = 0;
  edited.generateNormals = false;
  CHECK(engine::editor::save_import_settings(mesh.c_str(), edited),
        "the panel writes the edited settings");
  engine::content::AssetSidecar after{};
  CHECK(engine::content::read_asset_sidecar(mesh.c_str(), &after) ==
            engine::content::SidecarReadResult::Ok,
        "the sidecar still reads after the write");
  CHECK(after.guid == before.guid,
        "editing settings does not change the asset's identity");
  CHECK(after.hasMeshImport && (after.meshImport == edited),
        "the cook reads back exactly what the panel wrote");
  CHECK(!std::filesystem::exists(mesh + ".cookmeta"),
        "the panel never writes authored settings to the cooked record");

  // An asset with no sidecar is never given one by an edit: that would
  // hand it an identity nobody imported.
  CHECK(!engine::editor::save_import_settings(other.c_str(), edited),
        "settings cannot be saved onto an asset with no identity");
  CHECK(!std::filesystem::exists(other + ".meta"),
        "the refused save left no sidecar behind");

  // Thumbnails are one level, sampled without mipmaps (#549). They used to
  // ask for a generated chain the bgfx backend leaves empty, then draw it
  // smaller than stored, which sampled the empty levels: black icons.
  {
    const unsigned char texel[4] = {255U, 0U, 0U, 255U};
    const engine::renderer::TextureDesc thumb =
        engine::editor::thumbnail_texture_desc(1, 1, texel);
    CHECK(thumb.mipLevels == 1, "a thumbnail has exactly one level");
    CHECK(thumb.filter == engine::renderer::TextureFilter::Linear,
          "a thumbnail is not sampled through a mip chain");
    CHECK(thumb.wrap == engine::renderer::TextureWrap::ClampEdge,
          "a thumbnail's edges do not bleed into each other");
    CHECK((thumb.format == engine::renderer::TextureFormat::RGBA8) &&
              (thumb.width == 1) && (thumb.height == 1) &&
              (thumb.pixels == texel),
          "the decoded pixels are what is uploaded");
  }

  // Thumbnails: a miss is remembered.
  engine::editor::clear_thumbnail_cache();
  const std::string noThumb = dir + "/no_thumbnail.mesh";
  static_cast<void>(engine::editor::load_thumbnail_texture(noThumb.c_str()));
  CHECK(engine::editor::editor_session().thumbnailCount == 1U,
        "a missing thumbnail leaves a negative cache entry");
  static_cast<void>(engine::editor::load_thumbnail_texture(noThumb.c_str()));
  CHECK(engine::editor::editor_session().thumbnailCount == 1U,
        "the negative entry answers the next frame");
  engine::editor::clear_thumbnail_cache();
  CHECK(engine::editor::editor_session().thumbnailCount == 0U,
        "clearing forgets the negative entry");

  std::filesystem::remove_all(dir, ec);
  if (g_failures != 0) {
    std::fprintf(stderr, "editor_import_settings_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::puts("editor_import_settings_test passed");
  return 0;
}
