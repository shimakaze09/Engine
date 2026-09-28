// Verifies the engine's own content has a mount of its own: a default
// headless bootstrap mounts engine_assets/ at engine/ beside the game's
// assets/ at assets/, so the shader manifest, the editor font and the
// bootstrap mesh resolve under engine/ and not under assets/. An engine
// root that is missing or is a file refuses bootstrap at the mount with
// an Error naming it and rolls core back, as does an empty shader root;
// the next bootstrap with the defaults succeeds.

#include "../asset_root.h"
#include "engine/core/bootstrap.h"
#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"

#include <cstdio>
#include <cstring>

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
