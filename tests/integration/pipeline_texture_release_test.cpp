// Verifies that materials own their textures through the production
// pipeline (issue #663): a material the editor points at one texture after
// another, more distinct textures than the texture table holds, resolves
// every one, because each texture it stops naming gives back its record
// and its device texture; a texture another material still names keeps
// serving. Headless, on the null render device, through the editor's live
// material edit.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "engine/content/asset_catalog.h"
#include "engine/engine.h"
#include "engine/renderer/asset_database.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/editor_bridge.h"
#include "engine/runtime/engine_pipeline.h"

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

constexpr const char *kProject = "pipeline_texture_release_project";
constexpr const char *kSharedGuid = "0a7d3c5e-9b21-4f64-8e1d-2c4b6a8f0e13";
constexpr const char *kFirstGuid = "1b8e4d6f-0c32-4a75-9f2e-3d5c7b9a1f24";
constexpr const char *kEditedPath = "assets/materials/edited.mat";
constexpr const char *kKeeperPath = "assets/materials/keeper.mat";
/// More distinct textures than the table holds, so the edits exhaust it
/// unless every texture the material stops naming is freed.
constexpr std::size_t kTextures =
    engine::renderer::AssetDatabase::kMaxTextureAssets + 64U;

/// A 1x1 RGB PNG.
std::string png() {
  static constexpr unsigned char kPixel[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
      0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
      0x00, 0x03, 0x01, 0x01, 0x00, 0xC9, 0xFE, 0x92, 0xEF, 0x00, 0x00, 0x00,
      0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  return {reinterpret_cast<const char *>(kPixel), sizeof(kPixel)};
}

bool write_file(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

std::string meta(const char *guid) {
  return std::string("{\"schemaVersion\": 1, \"guid\": \"") + guid + "\"}\n";
}

std::string texture_name(std::size_t index) {
  return "t" + std::to_string(index) + ".png";
}

/// The id the catalog gives the index'th texture.
engine::content::AssetId texture_id(std::size_t index) {
  return engine::content::make_asset_id_from_path(
      ("assets/textures/" + texture_name(index)).c_str());
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

/// The bootstrap mesh from the repository, kTextures textures, a shared
/// texture, a material naming the first texture and one naming the
/// shared texture.
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
  const std::string pixel = png();
  for (std::size_t i = 0U; i < kTextures; ++i) {
    if (!write_file(root / "textures" / texture_name(i), pixel)) {
      return false;
    }
  }
  return write_file(root / "textures" / "t0.png.meta", meta(kFirstGuid)) &&
         write_file(root / "textures" / "shared.png", pixel) &&
         write_file(root / "textures" / "shared.png.meta", meta(kSharedGuid)) &&
         write_file(root / "materials" / "edited.mat",
                    std::string("{\"version\":4,\"textures\":{\"albedo\":\"") +
                        kFirstGuid + "\"}}") &&
         write_file(root / "materials" / "keeper.mat",
                    std::string("{\"version\":4,\"textures\":{\"albedo\":\"") +
                        kSharedGuid + "\"}}");
}

engine::renderer::TextureHandle albedo_of(const char *path) {
  return engine::runtime::editor_load_material(path).params.albedoTexture;
}

bool bridge_is_playing() noexcept { return false; }
bool bridge_is_paused() noexcept { return false; }

bool live(engine::renderer::TextureHandle handle) {
  return engine::renderer::texture_device_handle(handle) !=
         engine::renderer::kInvalidDeviceTexture;
}

void run_checks(engine::EnginePipeline &pipeline) {
  const engine::renderer::TextureHandle none =
      engine::renderer::kInvalidTextureHandle;
  g_tests.check(pipeline.execute_frame(), "the pipeline runs");
  const engine::renderer::TextureHandle shared = albedo_of(kKeeperPath);
  const engine::renderer::TextureHandle first = albedo_of(kEditedPath);
  g_tests.check((shared != none) && (first != none),
                "both materials start with their textures");

  // The material editor points the albedo slot at each texture in turn.
  std::size_t resolved = 1U;
  std::size_t stillLive = 0U;
  engine::renderer::TextureHandle previous = first;
  for (std::size_t i = 1U; i < kTextures; ++i) {
    const engine::runtime::EditorMaterialState state =
        engine::runtime::editor_load_material(kEditedPath);
    engine::renderer::MaterialTextureSlots slots = state.textureSlots;
    slots.albedo = texture_id(i);
    if (!state.found ||
        !engine::runtime::editor_set_material_params(state.materialId,
                                                     state.params, slots) ||
        !pipeline.execute_frame()) {
      break;
    }
    const engine::renderer::TextureHandle now = albedo_of(kEditedPath);
    resolved += ((now != none) && (now != previous)) ? 1U : 0U;
    stillLive += live(previous) ? 1U : 0U;
    previous = now;
  }
  char message[160] = {};
  std::snprintf(message, sizeof(message),
                "every one of %zu textures resolves in turn (%zu did)",
                kTextures, resolved);
  g_tests.check(resolved == kTextures, message);
  std::snprintf(message, sizeof(message),
                "each texture the material stops naming is released (%zu "
                "were not)",
                stillLive);
  g_tests.check(stillLive == 0U, message);
  g_tests.check(!live(first), "the material's first texture is released");
  g_tests.check((albedo_of(kKeeperPath) == shared) && live(shared),
                "a texture another material names keeps serving");
  g_tests.check(live(previous), "the texture in use keeps serving");
}

} // namespace

int main() {
  if (!write_project()) {
    g_tests.fail("write the project");
    return g_tests.finish("pipeline texture release tests");
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
  config.mainScriptPath = "pipeline_texture_release_missing_main.lua";
  config.editorScenePath = "pipeline_texture_release_missing.scene";
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
  return g_tests.finish("pipeline texture release tests");
}
