// Verifies the editor application is built to run without a terminal on
// each desktop platform, from the target's own build properties:
// on Windows the GUI subsystem (WIN32_EXECUTABLE), on macOS an .app bundle
// (MACOSX_BUNDLE), and on Linux a desktop entry beside the binary with
// Terminal=false that starts it in its own directory.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "../test_harness.h"

int main() {
  engine::tests::TestContext t;
#if defined(ENGINE_PLATFORM_WIN64)
  t.check(ENGINE_APP_WIN32_EXECUTABLE == 1,
          "the editor links for the Windows GUI subsystem");
#elif defined(ENGINE_PLATFORM_MACOS)
  t.check(ENGINE_APP_MACOSX_BUNDLE == 1,
          "the editor is built as an .app bundle");
#elif defined(ENGINE_PLATFORM_LINUX)
  std::ifstream stream(ENGINE_APP_DESKTOP_FILE);
  std::ostringstream text;
  text << stream.rdbuf();
  const std::string entry = text.str();
  t.check(!entry.empty(), "a desktop entry sits beside the editor");
  t.check(entry.find("Type=Application\n") != std::string::npos,
          "it describes an application");
  t.check(entry.find("Terminal=false\n") != std::string::npos,
          "it runs without a terminal");
  t.check(entry.find("Path=") != std::string::npos,
          "it starts the editor in its own directory");
#endif
  return t.finish("app_gui_target");
}
