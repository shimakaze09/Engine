// Regression for issue #234 (#168 M1): the production engine::bootstrap path
// must initialize the renderer texture registry — before this fix nothing in
// production ever did, so load_texture/register_external_texture (material
// file textures, scene-capture material bindings) silently returned invalid
// handles in every shipped run while unit tests initialized the system
// manually and passed. Drives the real engine::bootstrap()/engine::shutdown()
// entry points (the pipeline_tick_cadence_test.cpp pattern) across two full
// lifecycle rounds and asserts the registry opens and closes with them.

#include "engine/engine.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"

#include <cstdio>
#include <filesystem>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

/// Walks upward from the current path until the bundled assets are found
/// (same technique as pipeline_tick_cadence_test.cpp).
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

    if (std::filesystem::exists(normalized / "assets/main.lua", ec) &&
        std::filesystem::exists(normalized / "assets/shaders/default.vert",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

/// One bootstrap → registry-live → shutdown → registry-closed round.
void run_lifecycle_round(int round) noexcept {
  std::fprintf(stderr, "lifecycle round %d\n", round);

  if (!engine::bootstrap()) {
    std::fprintf(stderr, "FAIL: bootstrap (round %d)\n", round);
    ++g_failures;
    return;
  }

  // The production gate #234 left closed: registration must succeed after
  // bootstrap without any test-side initialize_texture_system call.
  const engine::renderer::DeviceTextureHandle deviceTexture{7U};
  const engine::renderer::TextureHandle handle =
      engine::renderer::register_external_texture(deviceTexture);
  CHECK(handle != engine::renderer::kInvalidTextureHandle,
        "register_external_texture succeeds after bootstrap");
  CHECK(engine::renderer::texture_device_handle(handle) == deviceTexture,
        "registered handle resolves to its device texture");

  // Boundary: invalid inputs still fail cleanly while the registry is live.
  CHECK(engine::renderer::load_texture(nullptr) ==
            engine::renderer::kInvalidTextureHandle,
        "load_texture(nullptr) stays invalid");

  engine::renderer::unload_texture(handle);
  CHECK(engine::renderer::texture_device_handle(handle) ==
            engine::renderer::kInvalidDeviceTexture,
        "unloaded handle no longer resolves");

  engine::shutdown();

  CHECK(engine::renderer::register_external_texture(deviceTexture) ==
            engine::renderer::kInvalidTextureHandle,
        "registry is closed again after engine::shutdown");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }

  // Pre-bootstrap the registry must reject work (the pinned uninitialized
  // contract from texture_loader_test.cpp).
  CHECK(engine::renderer::register_external_texture(
            engine::renderer::DeviceTextureHandle{7U}) ==
            engine::renderer::kInvalidTextureHandle,
        "registry rejects registration before bootstrap");

  // Two full rounds: the second proves shutdown left no latched state and
  // repeated bootstrap re-opens the registry (the #168 step-7 boundary).
  run_lifecycle_round(1);
  run_lifecycle_round(2);

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }

  std::puts("texture_system_bootstrap_test passed");
  return 0;
}
