// Verifies the engine's own content has a mount of its own: a default
// headless bootstrap mounts engine_assets/ at engine/ beside the game's
// assets/ at assets/, so the shader manifest, the editor font and the
// bootstrap mesh resolve under engine/ and not under assets/. An engine
// root that is missing or is a file refuses bootstrap at the mount with
// an Error naming it and rolls core back, as does an empty shader root;
// the next bootstrap with the defaults succeeds. A config that leaves the
// engine root empty finds it through ENGINE_ROOT first (a missing one is
// refused by name, even with engine_assets in the working directory),
// and an explicit root is used as given whatever ENGINE_ROOT says.

#include "../asset_root.h"
#include "engine/core/bootstrap.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;
int g_rootErrors = 0;

#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (false)

/// Counts the bootstrap's refusal of an engine content root.
void count_root_errors(engine::core::LogLevel level, const char *,
                       const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Error) && (message != nullptr) &&
      (std::strstr(message, "engine content root") != nullptr)) {
    ++g_rootErrors;
  }
}

/// Bootstraps headless with `engineRoot` and `shaderRoot`, expecting a
/// refusal, and checks it left nothing running and said why.
void check_refused(const char *engineRoot, const char *shaderRoot,
                   int expectedRootErrors, const char *what) noexcept {
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.engineRoot = engineRoot;
  config.shaderRootPath = shaderRoot;
  g_rootErrors = 0;
  const bool sinkOk =
      engine::core::log_register_sink(&count_root_errors, nullptr);
  const bool booted = engine::bootstrap(config);
  if (sinkOk) {
    engine::core::log_unregister_sink(&count_root_errors, nullptr);
  }
  CHECK(!booted, what);
  CHECK(!engine::core::is_core_initialized(),
        "a refused mount rolls core back");
  CHECK(g_rootErrors == expectedRootErrors,
        "the refusal names the engine content root when it is the cause");
  if (booted) {
    engine::shutdown();
  }
}

/// Sets or, for null, clears the ENGINE_ROOT environment variable.
bool set_engine_root_env(const char *value) {
#if defined(_WIN32)
  return _putenv_s("ENGINE_ROOT", (value != nullptr) ? value : "") == 0;
#else
  return (value != nullptr) ? (setenv("ENGINE_ROOT", value, 1) == 0)
                            : (unsetenv("ENGINE_ROOT") == 0);
#endif
}

/// The empty (automatic) engine root honours ENGINE_ROOT ahead of the
/// working directory's engine_assets, and an explicit root ignores it.
void check_engine_root_lookup() {
  CHECK(set_engine_root_env("engine_mount_test_env_missing"),
        "set ENGINE_ROOT to a missing directory");
  check_refused("", "engine/shaders", 1,
                "ENGINE_ROOT wins over the working directory, and a missing "
                "one is refused by name");

  const std::string engineRoot = engine::tests::engine_root_path();
  CHECK(!engineRoot.empty() && set_engine_root_env(engineRoot.c_str()),
        "set ENGINE_ROOT to the engine's content");
  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    CHECK(engine::bootstrap(config) &&
              engine::core::vfs_file_exists(
                  "engine/shaders/bgfx/shaders.manifest"),
          "an automatic root found through ENGINE_ROOT mounts the engine");
    engine::shutdown();
  }

  CHECK(set_engine_root_env("engine_mount_test_env_missing"),
        "point ENGINE_ROOT at a missing directory again");
  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.engineRoot = engineRoot.c_str();
    CHECK(engine::bootstrap(config),
          "an explicit engine root is used whatever ENGINE_ROOT says");
    engine::shutdown();
  }
  CHECK(set_engine_root_env(nullptr), "clear ENGINE_ROOT");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    CHECK(engine::bootstrap(config), "a default bootstrap mounts both roots");
    CHECK(engine::core::vfs_directory_exists("engine"),
          "engine/ is mounted");
    CHECK(engine::core::vfs_file_exists("engine/shaders/bgfx/shaders.manifest"),
          "the shader manifest resolves under engine/");
    CHECK(engine::core::vfs_file_exists("engine/fonts/Roboto-Medium.ttf"),
          "the editor font resolves under engine/");
    CHECK(engine::core::vfs_file_exists("engine/triangle.mesh"),
          "the bootstrap mesh resolves under engine/");
    CHECK(engine::core::vfs_file_exists("assets/main.lua"),
          "the game's content still resolves under assets/");
    CHECK(!engine::core::vfs_file_exists("assets/shaders/bgfx/shaders.manifest"),
          "the engine's shaders are no longer part of the game's content");
    engine::shutdown();
  }

  check_refused("engine_mount_test_missing_root", "engine/shaders", 1,
                "a missing engine root refuses bootstrap");
  check_refused("assets/main.lua", "engine/shaders", 1,
                "an engine root that is a file refuses bootstrap");
  check_refused("engine_assets", "", 0,
                "an empty shader root refuses bootstrap");
  check_engine_root_lookup();

  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    CHECK(engine::bootstrap(config),
          "the defaults bootstrap again after the refusals");
    engine::shutdown();
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "engine_mount_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("engine_mount_test: all checks passed\n");
  return 0;
}
