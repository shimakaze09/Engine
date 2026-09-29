// Verifies the editor application and the player are each built to run
// without a terminal on each desktop platform, from the targets' own build
// properties:
// on Windows the GUI subsystem (WIN32_EXECUTABLE), on macOS an .app bundle
// (MACOSX_BUNDLE), and on Linux a desktop entry beside the binary with
// Terminal=false that starts it in its own directory.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "../test_harness.h"

namespace {

/// One windowed executable's build properties.
struct GuiTarget final {
  const char *name;
  int win32Executable;
  int macosxBundle;
  const char *desktopFile;
};

void check_gui_target(engine::tests::TestContext &t,
                      const GuiTarget &target) noexcept {
  char what[160] = {};
#if defined(ENGINE_PLATFORM_WIN64)
  std::snprintf(what, sizeof(what), "%s links for the Windows GUI subsystem",
                target.name);
  t.check(target.win32Executable == 1, what);
#elif defined(ENGINE_PLATFORM_MACOS)
  std::snprintf(what, sizeof(what), "%s is built as an .app bundle",
                target.name);
  t.check(target.macosxBundle == 1, what);
#elif defined(ENGINE_PLATFORM_LINUX)
  std::ifstream stream(target.desktopFile);
  std::ostringstream text;
  text << stream.rdbuf();
  const std::string entry = text.str();
  std::snprintf(what, sizeof(what),
                "a desktop entry beside %s describes an application that "
                "runs without a terminal in its own directory",
                target.name);
  t.check(!entry.empty() &&
              (entry.find("Type=Application\n") != std::string::npos) &&
              (entry.find("Terminal=false\n") != std::string::npos) &&
              (entry.find("Path=") != std::string::npos),
          what);
#else
  static_cast<void>(t);
  static_cast<void>(target);
  static_cast<void>(what);
#endif
}

} // namespace

int main() {
  engine::tests::TestContext t;
  check_gui_target(
      t, GuiTarget{"engine_editor_app", ENGINE_APP_WIN32_EXECUTABLE,
                   ENGINE_APP_MACOSX_BUNDLE, ENGINE_APP_DESKTOP_FILE});
  check_gui_target(t, GuiTarget{"engine_player", ENGINE_PLAYER_WIN32_EXECUTABLE,
                                ENGINE_PLAYER_MACOSX_BUNDLE,
                                ENGINE_PLAYER_DESKTOP_FILE});
  return t.finish("app_gui_target");
}
