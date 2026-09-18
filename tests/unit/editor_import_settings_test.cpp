// Verifies the Assets panel's per-frame disk work is gone (#528): the
// import-settings sidecar is read once per selection change (and once
// more after an explicit invalidation), its fields parse exactly, a
// missing sidecar is a cached answer too, and a thumbnail that cannot be
// produced is remembered instead of being opened again on every frame.

#include "editor_import_settings.h"
#include "editor_session.h"

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
  std::FILE *file = std::fopen(path.c_str(), "wb");
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
  const std::string mesh = dir + "/thing.mesh";
  const std::string other = dir + "/other.mesh";
  CHECK(write_file(mesh, "binary"), "asset written");
  CHECK(write_file(mesh + ".meta.json",
                   "{\"schema\":1,\"importSettings\":{\"meshIndex\":2,"
                   "\"primitiveIndex\":3,\"scaleFactor\":0.5,\"upAxis\":2,"
                   "\"generateNormals\":true},\"outputs\":{}}"),
        "sidecar written");

  const std::uint64_t reads0 = engine::editor::import_settings_read_count();
  const ImportSettingsDocument *doc =
      engine::editor::import_settings_for_asset(mesh.c_str());
  CHECK(doc != nullptr, "document returned");
  CHECK(engine::editor::import_settings_read_count() == reads0 + 1U,
        "first selection reads the sidecar once");
  if (doc != nullptr) {
    CHECK(doc->state == ImportSettingsDocument::State::Valid, "sidecar valid");
    CHECK((doc->meshIndex == 2) && (doc->primitiveIndex == 3) &&
              (doc->scaleFactor == 0.5F) && (doc->upAxis == 2) &&
              doc->generateNormals,
          "fields parse exactly");
    CHECK(std::strstr(doc->document, "\"outputs\"") != nullptr,
          "the whole document is kept for the splice");
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
