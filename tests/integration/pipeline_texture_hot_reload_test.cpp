// Verifies texture hot reload through the production pipeline (issue
// #681): a project whose material names a texture that fails to load
// shows the texture once the fixed file is saved; an edit to a loaded
// texture reaches both the material naming it and the material
// inheriting it, and the replaced texture is released; a save that no
// longer loads keeps the previous texture serving. Headless, on the null
// render device, with an editor bridge published so the hot-reload stage
// polls. The poll runs on a wall-clock interval, so each wait is a
// bounded harness timeout; no elapsed time is asserted.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include "engine/core/engine_stats.h"
#include "engine/engine.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

constexpr const char *kProject = "pipeline_texture_hot_reload_project";
constexpr const char *kTextureGuid = "6f1c2a7e-3b9d-4e21-9a55-0c8d7e4b1f02";
constexpr const char *kTintedGuid = "b2e4d6f8-1a3c-4e5f-8a7b-9c0d1e2f3a4b";
constexpr const char *kInheritGuid = "c3f5e7a9-2b4d-4f6a-9b8c-0d1e2f3a4b5c";
constexpr const char *kTintedPath = "assets/materials/tinted.mat";
constexpr const char *kInheritPath = "assets/materials/inherits.mat";

/// A 1x1 RGB PNG; `green` picks the pixel, so the two versions differ.
std::string png(bool green) {
  static constexpr unsigned char kRed[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
      0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
      0x00, 0x03, 0x01, 0x01, 0x00, 0xC9, 0xFE, 0x92, 0xEF, 0x00, 0x00, 0x00,
      0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  static constexpr unsigned char kGreen[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
      0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0xF8, 0xCF, 0x00,
      0x00, 0x02, 0x02, 0x01, 0x00, 0x7B, 0x09, 0x81, 0x78, 0x00, 0x00, 0x00,
      0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  return green
             ? std::string(reinterpret_cast<const char *>(kGreen),
                           sizeof(kGreen))
             : std::string(reinterpret_cast<const char *>(kRed), sizeof(kRed));
}

bool write_file(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

std::filesystem::path texture_file() {
  return std::filesystem::path(kProject) / "textures" / "albedo.png";
}

/// Saves the texture and moves its time a whole second past the previous
/// one, so the save is visible however coarse the filesystem's clock.
bool save_texture(const std::string &bytes) {
  std::error_code ec{};
  const auto before = std::filesystem::last_write_time(texture_file(), ec);
  if (ec || !write_file(texture_file(), bytes)) {
    return false;
  }
  std::filesystem::last_write_time(texture_file(),
                                   before + std::chrono::seconds(1), ec);
  return !ec;
}

std::string meta(const char *guid) {
  return std::string("{\"schemaVersion\": 1, \"guid\": \"") + guid + "\"}\n";
}

/// The repository's assets directory, found by walking up from the
/// working directory; empty when it is not found.
std::filesystem::path repository_assets() {
  std::error_code ec{};
  std::filesystem::path dir = std::filesystem::current_path(ec);
  for (int depth = 0; !ec && (depth < 6); ++depth) {
    if (std::filesystem::exists(dir / "assets" / "triangle.mesh", ec)) {
      return dir / "assets";
    }
    dir = dir.parent_path();
  }
  return {};
}

/// cook records from the repository, a texture that does not load, a
/// cook stamp from the repository, a texture that does not load, a
/// material naming it and a material inheriting that one.
bool write_project() {
  std::error_code ec{};
  std::filesystem::remove_all(kProject, ec);
  const std::filesystem::path root(kProject);
  std::filesystem::create_directories(root / "textures", ec);
  std::filesystem::create_directories(root / "materials", ec);
  const std::filesystem::path assets = repository_assets();
  if (ec || assets.empty()) {
    return false;
  }
  for (const char *name :
       {"triangle.mesh", "triangle.mesh.cookmeta", "triangle.mesh.cookstamp"}) {
    std::filesystem::copy_file(assets / name, root / name, ec);
    if (ec) {
      return false;
    }
  }
  return write_file(texture_file(), "not a png") &&
         write_file(root / "textures" / "albedo.png.meta",
                    meta(kTextureGuid)) &&
         write_file(root / "materials" / "tinted.mat",
                    std::string("{\"version\":4,\"textures\":{\"albedo\":\"") +
                        kTextureGuid + "\"}}") &&
         write_file(root / "materials" / "tinted.mat.meta",
                    meta(kTintedGuid)) &&
         write_file(root / "materials" / "inherits.mat",
                    std::string("{\"version\":4,\"parent\":\"") + kTintedGuid +
                        "\",\"roughness\":0.9}") &&
         write_file(root / "materials" / "inherits.mat.meta",
                    meta(kInheritGuid));
}

engine::renderer::TextureHandle albedo_of(const char *path) {
  return engine::runtime::editor_load_material(path).params.albedoTexture;
}

bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

/// Runs frames until `done` holds after a frame, or the harness timeout.
template <typename Done>
bool run_until(engine::EnginePipeline &pipeline, Done done) {
  // wall-clock: harness-timeout
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  // wall-clock: harness-timeout
  while (std::chrono::steady_clock::now() < deadline) {
    if (!pipeline.execute_frame()) {
      return false;
    }
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

/// Runs frames until the hot-reload stage has swept every texture record
/// at least once since the call.
bool run_full_sweep(engine::EnginePipeline &pipeline) {
  std::uint32_t polls = 0U;
  return run_until(pipeline, [&polls]() {
    polls += engine::core::get_engine_stats().hotReloadPolls;
    // One poll checks every texture this project loads (fewer than
    // kTextureReloadPollSlots); the rest are margin.
    return polls > 8U;
  });
}

void run_checks(engine::EnginePipeline &pipeline) {
  const engine::renderer::TextureHandle none =
      engine::renderer::kInvalidTextureHandle;
  g_tests.check(run_full_sweep(pipeline), "the pipeline runs");
  g_tests.check(engine::runtime::editor_load_material(kTintedPath).found,
                "the project's material loaded");
  g_tests.check(albedo_of(kTintedPath) == none,
                "a texture that does not load leaves the slot empty");

  // Acceptance: a texture that failed loads once the fixed file is saved.
  g_tests.check(save_texture(png(false)), "save the fixed texture");
  const bool recovered =
      run_until(pipeline, [none]() { return albedo_of(kTintedPath) != none; });
  g_tests.check(recovered, "the fixed texture loads without a restart");
  const engine::renderer::TextureHandle first = albedo_of(kTintedPath);
  g_tests.check(albedo_of(kInheritPath) == first,
                "the inheriting material shows the recovered texture");

  // Acceptance: editing a texture re-resolves every material using it.
  g_tests.check(save_texture(png(true)), "save the edited texture");
  const bool edited = run_until(pipeline, [first, none]() {
    const engine::renderer::TextureHandle now = albedo_of(kTintedPath);
    return (now != first) && (now != none);
  });
  g_tests.check(edited, "the edited texture replaces the old one");
  const engine::renderer::TextureHandle second = albedo_of(kTintedPath);
  g_tests.check(albedo_of(kInheritPath) == second,
                "the inheriting material follows the edit");
  g_tests.check(engine::renderer::texture_device_handle(first) ==
                    engine::renderer::kInvalidDeviceTexture,
                "the replaced texture is released");

  // A save that no longer loads keeps the edited texture serving.
  g_tests.check(save_texture("not a png"), "save a broken texture");
  g_tests.check(run_full_sweep(pipeline), "the poll sweeps the table");
  g_tests.check(albedo_of(kTintedPath) == second &&
                    albedo_of(kInheritPath) == second,
                "a broken save leaves the previous texture in use");
  g_tests.check(engine::renderer::texture_device_handle(second) !=
                    engine::renderer::kInvalidDeviceTexture,
                "the serving texture is not released");
}

} // namespace

int main() {
  if (!write_project()) {
    g_tests.fail("write the project");
    return g_tests.finish("pipeline texture hot reload tests");
  }
  engine::runtime::EditorBridge bridge{};
  bridge.is_playing = &bridge_is_playing;
  bridge.is_paused = &bridge_is_paused;
  engine::runtime::set_editor_bridge(&bridge);

  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.assetMount = "assets";
  config.assetRoot = kProject;
  config.editorAssetRoot = kProject;
  config.mainScriptPath = "pipeline_texture_hot_reload_missing_main.lua";
  config.editorScenePath = "pipeline_texture_hot_reload_missing.scene";
  if (!engine::bootstrap(config)) {
    g_tests.fail("bootstrap");
  } else {
    {
      engine::EnginePipeline pipeline;
      if (!pipeline.initialize(0U)) {
        g_tests.fail("initialize the pipeline");
      } else {
        run_checks(pipeline);
      }
      pipeline.teardown();
    }
    engine::shutdown();
  }
  engine::runtime::set_editor_bridge(nullptr);

  std::error_code ec{};
  std::filesystem::remove_all(kProject, ec);
  return g_tests.finish("pipeline texture hot reload tests");
}
