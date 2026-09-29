// Regression for #342: engine::bootstrap must take its own copies of the
// configuration's borrowed strings. Before this fix EngineConfig's
// const char* paths and the window title were shallow-copied into the
// active configuration, so every later active_config() read — the asset
// mount, the player-mode scene path, the editor's asset root — walked the
// caller's buffers long after bootstrap returned. Drives the production
// engine::bootstrap entry point with heap-backed strings the caller then
// overwrites and frees, and covers the adoption boundaries: a null path,
// one character past the limit, a path exactly at the limit, and the
// rollback that keeps a previously adopted configuration intact. A
// configuration's package table is adopted the same way, and refused
// past its limit, without a table, without a project, or when a
// package's root is not a directory.

#include "../asset_root.h"
#include "engine/core/bootstrap.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

// Mirrors the limit in runtime/src/engine_config_strings.h, which is
// module-private; the boundary cases below fail loudly if the two drift.
constexpr std::size_t kMaxConfigStringLength = 259U;

int g_failures = 0;

#define CHECK(cond, msg)                                               \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);   \
      ++g_failures;                                                    \
    }                                                                  \
  } while (false)

/// True when a bootstrap was refused with nothing left running. One that
/// unexpectedly succeeds is shut down here, so a regression fails the
/// check instead of leaving the engine up for the rest of the test.
bool refused(bool booted) noexcept {
  if (booted) {
    engine::shutdown();
    return false;
  }
  return !engine::core::is_core_initialized();
}

/// Builds a heap copy of `text`, standing in for the dynamically built
/// path an embedder passes and then releases.
char *heap_string(const char *text) noexcept {
  const std::size_t size = std::strlen(text) + 1U;
  char *buffer = static_cast<char *>(std::malloc(size));
  if (buffer != nullptr) {
    std::memcpy(buffer, text, size);
  }
  return buffer;
}

/// Overwrites a heap string in place, so a retained pointer reads the
/// scribble instead of the path that was configured.
void scribble(char *buffer) noexcept {
  if (buffer == nullptr) {
    return;
  }
  for (char *cursor = buffer; *cursor != '\0'; ++cursor) {
    *cursor = 'X';
  }
}

/// Compares an adopted configuration string against what was configured,
/// treating a null as a mismatch rather than dereferencing it.
bool adopted_equals(const char *adopted, const char *expected) noexcept {
  return (adopted != nullptr) && (std::strcmp(adopted, expected) == 0);
}

/// Fills `buffer` with `length` filler characters plus a terminator.
void fill_path(char *buffer, std::size_t length, char filler) noexcept {
  for (std::size_t i = 0U; i < length; ++i) {
    buffer[i] = filler;
  }
  buffer[length] = '\0';
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!engine::tests::enter_asset_root()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  // --- Adopted strings outlive the caller's buffers. ---
  // Every value a rejection case later reads back differs from the
  // matching `EngineConfig` default, so an assertion that it survived
  // distinguishes preserved storage from storage overwritten with the
  // default's identical text. The mount pairs stay as the defaults because
  // bootstrap mounts them for real.
  char *assetMount = heap_string("assets");
  char *assetRoot = heap_string("assets");
  char *engineMount = heap_string("engine");
  const std::string engineRootPath = engine::tests::engine_root_path();
  char *engineRoot = heap_string(engineRootPath.c_str());
  char *mainScriptPath = heap_string("assets/probe_main.lua");
  char *bootstrapMeshPath = heap_string("assets/probe.mesh");
  char *shaderRootPath = heap_string("assets/probe_shaders");
  char *editorScenePath = heap_string("assets/probe_scene.json");
  char *editorAssetRoot = heap_string("assets/probe_root");
  char *windowTitle = heap_string("config strings probe");
  if ((assetMount == nullptr) || (assetRoot == nullptr) ||
      (engineMount == nullptr) || (engineRoot == nullptr) ||
      (mainScriptPath == nullptr) || (bootstrapMeshPath == nullptr) ||
      (shaderRootPath == nullptr) || (editorScenePath == nullptr) ||
      (editorAssetRoot == nullptr) || (windowTitle == nullptr)) {
    std::fprintf(stderr, "FAIL: could not allocate the caller's strings\n");
    return 1;
  }

  {
    // Headless bootstrap keeps the null render device standing in, so the
    // production entry point runs on every CI lane.
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.assetMount = assetMount;
    config.assetRoot = assetRoot;
    config.engineMount = engineMount;
    config.engineRoot = engineRoot;
    config.mainScriptPath = mainScriptPath;
    config.bootstrapMeshPath = bootstrapMeshPath;
    config.shaderRootPath = shaderRootPath;
    config.editorScenePath = editorScenePath;
    config.editorAssetRoot = editorAssetRoot;
    config.core.platform.title = windowTitle;

    if (!engine::bootstrap(config)) {
      std::fprintf(stderr, "FAIL: bootstrap with heap-backed paths\n");
      return 2;
    }

    const engine::EngineConfig &active = engine::active_config();
    CHECK(active.assetMount != assetMount,
          "the active configuration holds engine storage, not the caller's "
          "pointer");
    CHECK(active.core.platform.title != windowTitle,
          "the window title is engine storage, not the caller's pointer");

    scribble(assetMount);
    scribble(assetRoot);
    scribble(engineMount);
    scribble(engineRoot);
    scribble(mainScriptPath);
    scribble(bootstrapMeshPath);
    scribble(shaderRootPath);
    scribble(editorScenePath);
    scribble(editorAssetRoot);
    scribble(windowTitle);

    CHECK(adopted_equals(active.assetMount, "assets"),
          "assetMount survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.assetRoot, "assets"),
          "assetRoot survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.engineMount, "engine"),
          "engineMount survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.engineRoot, engineRootPath.c_str()),
          "engineRoot survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.mainScriptPath, "assets/probe_main.lua"),
          "mainScriptPath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.bootstrapMeshPath, "assets/probe.mesh"),
          "bootstrapMeshPath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.shaderRootPath, "assets/probe_shaders"),
          "shaderRootPath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.editorScenePath, "assets/probe_scene.json"),
          "editorScenePath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.editorAssetRoot, "assets/probe_root"),
          "editorAssetRoot survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.core.platform.title, "config strings probe"),
          "the window title survives the caller overwriting its buffer");

    // Releasing the caller's buffers is the heap-use-after-free the
    // sanitizer lane catches when the configuration only borrowed them.
    std::free(assetMount);
    std::free(assetRoot);
    std::free(engineMount);
    std::free(engineRoot);
    std::free(mainScriptPath);
    std::free(bootstrapMeshPath);
    std::free(shaderRootPath);
    std::free(editorScenePath);
    std::free(editorAssetRoot);
    std::free(windowTitle);

    CHECK(adopted_equals(active.assetMount, "assets"),
          "assetMount survives the caller freeing its buffer");
    CHECK(adopted_equals(active.mainScriptPath, "assets/probe_main.lua"),
          "mainScriptPath survives the caller freeing its buffer");
    CHECK(adopted_equals(active.editorScenePath, "assets/probe_scene.json"),
          "editorScenePath survives the caller freeing its buffer");
    CHECK(adopted_equals(active.core.platform.title, "config strings probe"),
          "the window title survives the caller freeing its buffer");

    engine::shutdown();
  }

  // --- A path one character past the limit is rejected, and the fields
  // adopted before it keep the values they were adopted with. ---
  {
    char overlong[kMaxConfigStringLength + 2U] = {};
    fill_path(overlong, kMaxConfigStringLength + 1U, 'a');

    // Only the over-long field is set, so the field the assertion below
    // reads carries its default — different text from what the block
    // above adopted. An implementation that wrote each field into the
    // live storage as it validated would leave that default behind,
    // which is what the mainScriptPath assertion catches.
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.bootstrapMeshPath = overlong;

    const bool booted = engine::bootstrap(config);
    CHECK(!booted, "a path one character past the limit is rejected");
    CHECK(!engine::core::is_core_initialized(),
          "a rejected configuration initializes no subsystem");
    CHECK(adopted_equals(engine::active_config().mainScriptPath,
                         "assets/probe_main.lua"),
          "rejection leaves a field validated before it untouched");
    CHECK(adopted_equals(engine::active_config().bootstrapMeshPath,
                         "assets/probe.mesh"),
          "rejection keeps the rejected field's adopted value");
    if (booted) {
      engine::shutdown();
    }
  }

  // --- A null path is reported instead of dereferenced, and failing on
  // the last field still leaves every earlier one untouched. ---
  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.editorAssetRoot = nullptr;

    const bool booted = engine::bootstrap(config);
    CHECK(!booted, "a null path is rejected");
    CHECK(!engine::core::is_core_initialized(),
          "a null path initializes no subsystem");
    // The widest partial-write window: a failure on the final field
    // means every earlier one had already passed validation.
    const engine::EngineConfig &active = engine::active_config();
    CHECK(adopted_equals(active.mainScriptPath, "assets/probe_main.lua"),
          "a late rejection leaves mainScriptPath untouched");
    CHECK(adopted_equals(active.shaderRootPath, "assets/probe_shaders"),
          "a late rejection leaves shaderRootPath untouched");
    CHECK(adopted_equals(active.editorScenePath, "assets/probe_scene.json"),
          "a late rejection leaves editorScenePath untouched");
    CHECK(adopted_equals(active.editorAssetRoot, "assets/probe_root"),
          "a late rejection keeps the rejected field's adopted value");
    if (booted) {
      engine::shutdown();
    }
  }

  // --- A null mount is rejected before any subsystem starts. ---
  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.assetMount = nullptr;

    const bool booted = engine::bootstrap(config);
    CHECK(!booted, "a null mount is rejected");
    CHECK(!engine::core::is_core_initialized(),
          "a null mount initializes no subsystem");
    CHECK(adopted_equals(engine::active_config().assetMount, "assets"),
          "a null mount leaves the previously adopted mount in place");
    if (booted) {
      engine::shutdown();
    }
  }

  // --- A path exactly at the limit is stored whole. ---
  {
    char maxPath[kMaxConfigStringLength + 1U] = {};
    fill_path(maxPath, kMaxConfigStringLength, 'b');

    // bootstrapMeshPath is read when a pipeline loads its bootstrap
    // content, not during bootstrap, so the limit case boots without
    // needing an asset of that name on disk.
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.bootstrapMeshPath = maxPath;

    if (!engine::bootstrap(config)) {
      std::fprintf(stderr, "FAIL: bootstrap with a maximum-length path\n");
      return 2;
    }

    const char *adopted = engine::active_config().bootstrapMeshPath;
    CHECK(adopted_equals(adopted, maxPath),
          "a path at the limit is stored whole");
    CHECK((adopted != nullptr) &&
              (std::strlen(adopted) == kMaxConfigStringLength),
          "the stored path keeps every character of the limit");

    engine::shutdown();
  }

  // --- A package table is adopted like every other string. ---
  {
    std::error_code ec{};
    const std::filesystem::path packageDir =
        std::filesystem::temp_directory_path(ec) /
        "engine_config_strings_package";
    std::filesystem::create_directories(packageDir, ec);
    const std::string packageRootText = packageDir.string();
    char *packageMount = heap_string("packages/probe");
    char *packageRoot = heap_string(packageRootText.c_str());
    engine::ContentMount *table = static_cast<engine::ContentMount *>(
        std::malloc(sizeof(engine::ContentMount)));
    if ((packageMount == nullptr) || (packageRoot == nullptr) ||
        (table == nullptr) || ec) {
      std::fprintf(stderr, "FAIL: could not build the package table\n");
      return 1;
    }
    table[0] = engine::ContentMount{packageMount, packageRoot};
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.packages = table;
    config.packageCount = 1U;
    const bool booted = engine::bootstrap(config);
    CHECK(booted, "a configuration with a package boots");
    if (booted) {
      const engine::EngineConfig &active = engine::active_config();
      CHECK(active.packages != table,
            "the package table is engine storage, not the caller's");
      scribble(packageMount);
      scribble(packageRoot);
      std::free(packageMount);
      std::free(packageRoot);
      std::free(table);
      CHECK(
          (active.packageCount == 1U) &&
              adopted_equals(active.packages[0].mount, "packages/probe") &&
              adopted_equals(active.packages[0].root, packageRootText.c_str()),
          "a package survives the caller freeing its strings and table");
      CHECK(engine::core::vfs_directory_exists("packages/probe"),
            "the package is mounted at its prefix");
      engine::shutdown();
    } else {
      std::free(packageMount);
      std::free(packageRoot);
      std::free(table);
    }

    engine::ContentMount one[1] = {{"packages/probe", packageRootText.c_str()}};
    engine::EngineConfig tooMany{};
    tooMany.core.platform.headless = true;
    tooMany.packages = one;
    tooMany.packageCount = engine::kMaxPackageMounts + 1U;
    CHECK(refused(engine::bootstrap(tooMany)),
          "a package count past the limit is refused before anything starts");

    engine::EngineConfig noTable{};
    noTable.core.platform.headless = true;
    noTable.packageCount = 1U;
    CHECK(refused(engine::bootstrap(noTable)),
          "a package count with no table is refused");

    engine::EngineConfig noProject{};
    noProject.core.platform.headless = true;
    noProject.assetRoot = "";
    noProject.mainScriptPath = "";
    noProject.editorScenePath = "";
    noProject.editorAssetRoot = "";
    noProject.packages = one;
    noProject.packageCount = 1U;
    CHECK(refused(engine::bootstrap(noProject)),
          "packages with no project open are refused");

    const std::string absent = (packageDir / "absent").string();
    engine::ContentMount missing[1] = {{"packages/probe", absent.c_str()}};
    engine::EngineConfig missingRoot{};
    missingRoot.core.platform.headless = true;
    missingRoot.packages = missing;
    missingRoot.packageCount = 1U;
    CHECK(refused(engine::bootstrap(missingRoot)),
          "a package whose root is not a directory refuses the bootstrap, "
          "rolled back");
    std::filesystem::remove_all(packageDir, ec);
  }

  std::fprintf(stdout, "engine_config_strings_test: %d failures\n",
               g_failures);
  return (g_failures == 0) ? 0 : 1;
}
