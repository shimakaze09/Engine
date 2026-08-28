// Regression for #342: engine::bootstrap must take its own copies of the
// configuration's borrowed strings. Before this fix EngineConfig's seven
// const char* paths and the window title were shallow-copied into the
// active configuration, so every later active_config() read — the asset
// mount, the player-mode scene path, the editor's asset root — walked the
// caller's buffers long after bootstrap returned. Drives the production
// engine::bootstrap entry point with heap-backed strings the caller then
// overwrites and frees, and covers the adoption boundaries: a null path,
// one character past the limit, a path exactly at the limit, and the
// rollback that keeps a previously adopted configuration intact.

#include "engine/core/bootstrap.h"
#include "engine/engine.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

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

/// Locates the bundled assets so the configured mount resolves, matching
/// the other bootstrap-driven integration tests.
bool set_working_directory_with_assets() noexcept {
  const std::filesystem::path original = std::filesystem::current_path();
  const std::filesystem::path candidates[] = {
      original, original / "..", original / "../..", original / "../../..",
      original / "../../../.."};

  for (const std::filesystem::path &candidate : candidates) {
    std::error_code ec{};
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
      continue;
    }

    if (std::filesystem::exists(normalized / "assets/main.lua", ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
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
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  // --- Adopted strings outlive the caller's buffers. ---
  char *assetMount = heap_string("assets");
  char *assetRoot = heap_string("assets");
  char *mainScriptPath = heap_string("assets/main.lua");
  char *bootstrapMeshPath = heap_string("assets/triangle.mesh");
  char *shaderRootPath = heap_string("assets/shaders");
  char *editorScenePath = heap_string("assets/scene.json");
  char *editorAssetRoot = heap_string("assets");
  char *windowTitle = heap_string("config strings probe");
  if ((assetMount == nullptr) || (assetRoot == nullptr) ||
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
    CHECK(adopted_equals(active.mainScriptPath, "assets/main.lua"),
          "mainScriptPath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.bootstrapMeshPath, "assets/triangle.mesh"),
          "bootstrapMeshPath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.shaderRootPath, "assets/shaders"),
          "shaderRootPath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.editorScenePath, "assets/scene.json"),
          "editorScenePath survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.editorAssetRoot, "assets"),
          "editorAssetRoot survives the caller overwriting its buffer");
    CHECK(adopted_equals(active.core.platform.title, "config strings probe"),
          "the window title survives the caller overwriting its buffer");

    // Releasing the caller's buffers is the heap-use-after-free the
    // sanitizer lane catches when the configuration only borrowed them.
    std::free(assetMount);
    std::free(assetRoot);
    std::free(mainScriptPath);
    std::free(bootstrapMeshPath);
    std::free(shaderRootPath);
    std::free(editorScenePath);
    std::free(editorAssetRoot);
    std::free(windowTitle);

    CHECK(adopted_equals(active.assetMount, "assets"),
          "assetMount survives the caller freeing its buffer");
    CHECK(adopted_equals(active.mainScriptPath, "assets/main.lua"),
          "mainScriptPath survives the caller freeing its buffer");
    CHECK(adopted_equals(active.editorScenePath, "assets/scene.json"),
          "editorScenePath survives the caller freeing its buffer");
    CHECK(adopted_equals(active.core.platform.title, "config strings probe"),
          "the window title survives the caller freeing its buffer");

    engine::shutdown();
  }

  // --- A path one character past the limit is rejected, and rejection
  // leaves the previously adopted configuration untouched. ---
  {
    char overlong[kMaxConfigStringLength + 2U] = {};
    fill_path(overlong, kMaxConfigStringLength + 1U, 'a');

    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.bootstrapMeshPath = overlong;

    const bool booted = engine::bootstrap(config);
    CHECK(!booted, "a path one character past the limit is rejected");
    CHECK(!engine::core::is_core_initialized(),
          "a rejected configuration initializes no subsystem");
    CHECK(adopted_equals(engine::active_config().bootstrapMeshPath,
                         "assets/triangle.mesh"),
          "rejection keeps the previously adopted paths");
    if (booted) {
      engine::shutdown();
    }
  }

  // --- A null path is reported instead of dereferenced. ---
  {
    engine::EngineConfig config{};
    config.core.platform.headless = true;
    config.assetMount = nullptr;

    const bool booted = engine::bootstrap(config);
    CHECK(!booted, "a null path is rejected");
    CHECK(!engine::core::is_core_initialized(),
          "a null path initializes no subsystem");
    CHECK(adopted_equals(engine::active_config().assetMount, "assets"),
          "a null path leaves the previously adopted mount in place");
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

  std::fprintf(stdout, "engine_config_strings_test: %d failures\n",
               g_failures);
  return (g_failures == 0) ? 0 : 1;
}
