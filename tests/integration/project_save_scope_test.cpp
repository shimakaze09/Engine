// Verifies that game saves are scoped per project: two projects on one
// machine, bootstrapped in turn through the production entry point, each
// save and load through the runtime save slot, and neither reads nor
// replaces the other's file. Also covers the same project named by a
// relative and an absolute root, a save.json left in the shared directory
// by an engine that predates the scoping (never read, never touched), and
// a save attempted with no project named (refused). The rebound input map
// the engine restores at startup lives in the same per-project directory,
// so the two projects' bindings paths differ too. The run gets a user
// profile of its own, so the developer's saves are never touched.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "engine/core/input_map.h"
#include "engine/core/platform.h"
#include "engine/engine.h"
#include "engine/runtime/save_data.h"

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

constexpr const char *kProfileRoot = "project_save_scope_profile";
constexpr const char *kProjectA = "project_save_scope_a";
constexpr const char *kProjectB = "project_save_scope_b";
constexpr const char *kSaveA = "{\"project\":\"a\"}";
constexpr const char *kSaveB = "{\"project\":\"b\"}";
constexpr const char *kLegacySave = "{\"project\":\"unknown\"}";

/// Points the per-user save directory at a fresh directory of the run's
/// own, the same way on every platform the engine derives it.
bool redirect_user_profile() {
  std::error_code ec{};
  const std::filesystem::path root =
      std::filesystem::absolute(std::filesystem::path(kProfileRoot), ec);
  if (ec) {
    return false;
  }
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return false;
  }
  const std::string text = root.string();
#if defined(_WIN32)
  return _putenv_s("APPDATA", text.c_str()) == 0;
#elif defined(__APPLE__)
  return setenv("HOME", text.c_str(), 1) == 0;
#else
  return setenv("XDG_DATA_HOME", text.c_str(), 1) == 0;
#endif
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

/// Bootstraps headless with `root` as the project's mounted content root.
bool bootstrap_project(const char *root) {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.assetMount = "assets";
  config.assetRoot = root;
  config.mainScriptPath = "project_save_scope_missing_main.lua";
  config.bootstrapMeshPath = "project_save_scope_missing.mesh";
  config.editorScenePath = "project_save_scope_missing.scene";
  config.editorAssetRoot = root;
  return engine::bootstrap(config);
}

/// What the running project's save slot holds; "<none>" when it is empty.
std::string load_slot() {
  char buffer[256] = {};
  std::size_t length = 0U;
  if (!engine::runtime::load_game_data(buffer, sizeof(buffer), &length)) {
    return "<none>";
  }
  return std::string(buffer, length);
}

/// One bootstrap of `root`: checks what the slot holds on arrival, then
/// saves `write` when it is not null. `bindingsPath`, when given, receives
/// the project's default input-bindings path.
void visit(const char *root, const char *expected, const char *write,
           const char *what, std::string *bindingsPath = nullptr) {
  if (!bootstrap_project(root)) {
    g_tests.fail(what);
    return;
  }
  if (bindingsPath != nullptr) {
    char path[1024] = {};
    g_tests.check(engine::core::input_bindings_default_path(path, sizeof(path)),
                  "the project's bindings path resolves");
    *bindingsPath = path;
  }
  const std::string loaded = load_slot();
  if (loaded != expected) {
    std::fprintf(stderr, "  %s: loaded %s, expected %s\n", what, loaded.c_str(),
                 expected);
  }
  g_tests.check(loaded == expected, what);
  if (write != nullptr) {
    g_tests.check(engine::runtime::save_game_data(write, std::strlen(write)),
                  "the project's save commits");
  }
  engine::shutdown();
}

} // namespace

int main() {
  if (!redirect_user_profile()) {
    g_tests.fail("redirect the user profile");
    return g_tests.finish("project save scope tests");
  }
  std::error_code ec{};
  for (const char *project : {kProjectA, kProjectB}) {
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);
  }

  // A save.json in the shared directory, as an engine without per-project
  // saves left it: it names no project, so no project may read it.
  char sharedDir[1024] = {};
  g_tests.check(
      engine::core::platform_get_save_dir(sharedDir, sizeof(sharedDir)),
      "the shared save directory resolves");
  const std::filesystem::path legacy =
      std::filesystem::path(sharedDir) / "save.json";
  std::filesystem::create_directories(legacy.parent_path(), ec);
  {
    std::ofstream stream(legacy, std::ios::binary);
    stream << kLegacySave;
  }

  std::string bindingsA{};
  std::string bindingsB{};
  visit(kProjectA, "<none>", kSaveA,
        "project A starts empty, not with the unattributed legacy save",
        &bindingsA);
  visit(kProjectB, "<none>", kSaveB, "project B does not see A's save",
        &bindingsB);
  g_tests.check(!bindingsA.empty() && (bindingsA != bindingsB),
                "the two projects' input bindings live apart");
  visit(kProjectA, kSaveA, nullptr, "project A still holds its own save");
  visit(kProjectB, kSaveB, nullptr, "project B holds its own save");

  const std::filesystem::path absoluteA =
      std::filesystem::absolute(std::filesystem::path(kProjectA), ec);
  const std::string absoluteAText = absoluteA.string();
  visit(absoluteAText.c_str(), kSaveA, nullptr,
        "an absolute root names the same project as its relative spelling");

  g_tests.check(read_file(legacy) == kLegacySave,
                "the legacy shared save is left byte-for-byte");

  // No bootstrap, no project: the save is refused rather than written to
  // the directory every project shares.
  g_tests.check(!engine::runtime::save_game_data(kSaveA, std::strlen(kSaveA)),
                "a save with no project named is refused");
  g_tests.check(read_file(legacy) == kLegacySave,
                "the refused save did not reach the shared directory");

  for (const char *project : {kProjectA, kProjectB}) {
    std::filesystem::remove_all(project, ec);
  }
  std::filesystem::remove_all(kProfileRoot, ec);
  return g_tests.finish("project save scope tests");
}
