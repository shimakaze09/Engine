// A one-shot driver for the editor-to-cook integration test: it calls
// the Assets panel's real save entry point, so the test changes an
// import setting the way an author does rather than by writing the
// sidecar bytes itself. That is the whole point of the test — the panel
// and the cook must agree on which file holds authored settings, and a
// test that writes the file directly would pass even if they did not.
//
// Usage: editor_import_settings_writer <source-asset> <scaleFactor>

#include <cstdio>
#include <cstdlib>

#include "editor_import_settings.h"
#include "engine/content/asset_sidecar.h"
#include "engine/core/logging.h"

/// Runs this executable or test program.
int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: editor_import_settings_writer <asset> <scale>\n");
    return 2;
  }
  const char *assetPath = argv[1];
  const double scale = std::strtod(argv[2], nullptr);

  static_cast<void>(engine::core::initialize_logging());

  // Read through the panel's own cache so the driver exercises the same
  // path the UI does, then edit only the field under test.
  const engine::editor::ImportSettingsDocument *document =
      engine::editor::import_settings_for_asset(assetPath);
  if ((document == nullptr) ||
      (document->state !=
       engine::editor::ImportSettingsDocument::State::Valid)) {
    std::fprintf(stderr,
                 "editor_import_settings_writer: %s has no readable "
                 "sidecar to edit\n",
                 assetPath);
    engine::core::shutdown_logging();
    return 3;
  }

  engine::content::MeshImportSettings edited = document->settings;
  edited.scaleFactor = static_cast<float>(scale);
  const bool saved = engine::editor::save_import_settings(assetPath, edited);
  engine::core::shutdown_logging();
  if (!saved) {
    std::fprintf(stderr, "editor_import_settings_writer: save failed\n");
    return 4;
  }
  std::printf("editor wrote scaleFactor %g to %s\n", scale, assetPath);
  return 0;
}
