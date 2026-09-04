// Regression test for #353: engine::shutdown must close the texture
// registry while the render device is still live, so the registry releases
// the device textures it owns itself instead of relying on the backend's
// mass teardown to reclaim them. Drives the production engine::bootstrap /
// EnginePipeline / engine::shutdown entry points headlessly on the null
// device and reads the registry's own diagnostic through the public log
// sink: a registry closed after the device reports the owned textures it
// could no longer release, so the ordering defect is observable without a
// spy device inside the production link.

#include "engine/core/logging.h"
#include "engine/core/vfs.h"
#include "engine/engine.h"
#include "engine/renderer/render_device.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/world.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

constexpr const char *kMountPrefix = "tos_tex";
constexpr const char *kTexturePath = "tos_tex/texture_owner_shutdown.png";
constexpr const char *kTextureOsPath = "texture_owner_shutdown.png";

// 1x1 transparent RGBA PNG.
constexpr unsigned char kTinyPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
    0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
    0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
    0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60,
    0x82};

int g_failures = 0;
int g_unreleasedWarnings = 0;

#define CHECK(cond, msg)                                                     \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__);         \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

void capture_world(engine::runtime::World *) noexcept {}
bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

/// Counts the registry's "closed after the render device" report.
void count_unreleased_warning(engine::core::LogLevel level, const char *,
                              const char *message, void *) noexcept {
  if ((level == engine::core::LogLevel::Warning) && (message != nullptr) &&
      (std::strstr(message, "texture registry closed after the render "
                            "device") != nullptr)) {
    ++g_unreleasedWarnings;
  }
}

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
        std::filesystem::exists(normalized / "assets/shaders/bgfx/shaders.json",
                                ec)) {
      std::filesystem::current_path(normalized, ec);
      return !ec;
    }
  }

  return false;
}

/// One bootstrap → run → shutdown round; `loadTexture` decides whether the
/// registry owns a device texture when the engine tears down.
void run_lifecycle_round(int round, bool loadTexture) noexcept {
  std::fprintf(stderr, "lifecycle round %d\n", round);
  g_unreleasedWarnings = 0;

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  if (!engine::bootstrap(config)) {
    std::fprintf(stderr, "FAIL: bootstrap (round %d)\n", round);
    ++g_failures;
    return;
  }

  // The pipeline run creates the (null) render device exactly as a
  // headless production run does; the device outlives the run because its
  // lifetime is the engine tier's.
  {
    engine::EnginePipeline pipeline;
    if (!pipeline.initialize(0U)) {
      std::fprintf(stderr, "FAIL: pipeline initialize (round %d)\n", round);
      ++g_failures;
      pipeline.teardown();
      engine::shutdown();
      return;
    }
    pipeline.teardown();
  }
  CHECK(engine::renderer::render_device() != nullptr,
        "the device is live after the run");

  if (loadTexture) {
    // A texture the registry owns, backed by a device texture it must
    // release itself. The file lives under a test-owned mount so the
    // bundled assets stay untouched.
    CHECK(engine::core::mount(kMountPrefix, "."), "mount the texture dir");
    CHECK(engine::core::vfs_write_binary(kTexturePath, kTinyPng,
                                         sizeof(kTinyPng)),
          "write the texture file");
    const engine::renderer::TextureHandle handle =
        engine::renderer::load_texture(kTexturePath);
    CHECK(handle != engine::renderer::kInvalidTextureHandle,
          "load a registry-owned texture");
    CHECK(engine::renderer::texture_device_handle(handle) !=
              engine::renderer::kInvalidDeviceTexture,
          "guard: the registry holds a device texture to release");
  }

  engine::shutdown();

  CHECK(engine::renderer::render_device() == nullptr,
        "shutdown released the device");
  CHECK(engine::renderer::register_external_texture(
            engine::renderer::DeviceTextureHandle{7U}) ==
            engine::renderer::kInvalidTextureHandle,
        "shutdown closed the registry");
  CHECK(g_unreleasedWarnings == 0,
        "the registry closed while the device was live, so it released "
        "its own textures and reported nothing unreleased");
}

} // namespace

/// Runs this executable or test program.
int main() {
  if (!set_working_directory_with_assets()) {
    std::fprintf(stderr, "FAIL: could not locate bundled assets\n");
    return 1;
  }
  if (!engine::core::initialize_logging() ||
      !engine::core::log_register_sink(&count_unreleased_warning, nullptr)) {
    std::fprintf(stderr, "FAIL: log sink\n");
    return 1;
  }

  engine::runtime::EditorBridge bridge{};
  bridge.set_world = &capture_world;
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  // Round 1 is the finding: a loaded texture must be released by the
  // registry before the device goes. Round 2 is the boundary: an empty
  // registry closes quietly, and the second bootstrap sees no latched
  // state from the first.
  run_lifecycle_round(1, true);
  run_lifecycle_round(2, false);

  engine::core::log_unregister_sink(&count_unreleased_warning, nullptr);
  static_cast<void>(std::remove(kTextureOsPath));

  if (g_failures != 0) {
    std::fprintf(stderr, "texture_owner_shutdown_order_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("texture_owner_shutdown_order_test: all checks passed\n");
  return 0;
}
