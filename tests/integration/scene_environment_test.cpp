// Verifies that a scene's sky light reaches the renderer through the
// production pipeline (issue #580): a scene that names a catalogued
// environment map, saved and loaded through the scene serializer, becomes
// the renderer's environment cubemap; the map loads once, not every frame;
// naming another map swaps it and releases the old one; naming an asset
// that is not an environment map, or removing the sky light, leaves the
// scene with no environment and releases what it had. Headless, on the
// null render device.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "engine/content/asset_catalog.h"
#include "engine/content/asset_identity.h"
#include "engine/engine.h"
#include "engine/renderer/command_buffer.h"
#include "engine/renderer/texture_loader.h"
#include "engine/runtime/engine_pipeline.h"
#include "engine/runtime/scene_serializer.h"
#include "engine/runtime/world.h"

#include "../test_harness.h"

namespace {

engine::tests::TestContext g_tests{};

constexpr const char *kProject = "scene_environment_project";
constexpr const char *kSkyGuid = "3c6e1f2a-8b4d-4e7f-9a1c-5d2e8f4b6a01";
constexpr const char *kDuskGuid = "4d7f2a3b-9c5e-4f80-8b2d-6e3f9a5c7b12";
constexpr const char *kTextureGuid = "5e8a3b4c-ad6f-4091-9c3e-7f4a0b6d8c23";
constexpr const char *kSkyPath = "assets/environments/sky.hdr";
constexpr const char *kDuskPath = "assets/environments/dusk.hdr";

/// A 8x4 Radiance file whose every pixel is `level` (RGBE, exponent 129,
/// so 128 is 1.0).
std::string radiance(unsigned char level) {
  std::string bytes = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
  for (int i = 0; i < 32; ++i) {
    bytes.push_back(static_cast<char>(level));
    bytes.push_back(static_cast<char>(level));
    bytes.push_back(static_cast<char>(level));
    bytes.push_back(static_cast<char>(129));
  }
  return bytes;
}

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

/// The bootstrap mesh from the repository, two environment maps and a
/// texture, each with the sidecar that gives it its identity.
bool write_project() {
  std::error_code ec{};
  std::filesystem::remove_all(kProject, ec);
  const std::filesystem::path root(kProject);
  std::filesystem::create_directories(root / "environments", ec);
  std::filesystem::create_directories(root / "textures", ec);
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
  return write_file(root / "environments" / "sky.hdr", radiance(128)) &&
         write_file(root / "environments" / "sky.hdr.meta", meta(kSkyGuid)) &&
         write_file(root / "environments" / "dusk.hdr", radiance(64)) &&
         write_file(root / "environments" / "dusk.hdr.meta",
                    meta(kDuskGuid)) &&
         write_file(root / "textures" / "flat.png", png()) &&
         write_file(root / "textures" / "flat.png.meta", meta(kTextureGuid));
}

engine::core::AssetRef ref_of(const char *guid) {
  engine::core::AssetRef ref{};
  static_cast<void>(engine::content::parse_asset_ref(guid, &ref));
  return ref;
}

bool live(engine::renderer::TextureHandle handle) {
  return engine::renderer::texture_device_handle(handle) !=
         engine::renderer::kInvalidDeviceTexture;
}

/// Points the World's one sky light at `guid` and runs a frame.
bool name_environment(engine::EnginePipeline &pipeline,
                      engine::runtime::World &world,
                      engine::runtime::Entity entity, const char *guid) {
  engine::runtime::SkyLightComponent *skyLight =
      world.get_sky_light_component_ptr(entity);
  if (skyLight == nullptr) {
    return false;
  }
  skyLight->environmentRef = ref_of(guid);
  return pipeline.execute_frame();
}

void run_checks(engine::EnginePipeline &pipeline,
                engine::runtime::World &world) {
  using engine::renderer::get_skybox_texture;
  const engine::renderer::TextureHandle none =
      engine::renderer::kInvalidTextureHandle;
  g_tests.check(pipeline.execute_frame(), "the pipeline runs");
  g_tests.check(get_skybox_texture() == none,
                "a scene with no sky light has no environment");

  // Author the sky light, then take the scene through the serializer, so
  // the pipeline binds what a saved scene carries.
  const engine::runtime::Entity authored = world.create_scene_object();
  engine::runtime::SkyLightComponent sky{};
  sky.environmentRef = ref_of(kSkyGuid);
  static char scene[64U * 1024U] = {};
  std::size_t size = 0U;
  g_tests.check((authored != engine::runtime::kInvalidEntity) &&
                    world.add_sky_light_component(authored, sky) &&
                    engine::runtime::save_scene(world, scene, sizeof(scene),
                                                &size) &&
                    engine::runtime::load_scene(world, scene, size),
                "a scene naming an environment saves and loads");
  g_tests.check(std::string(scene, size).find(kSkyGuid) != std::string::npos,
                "the saved scene names the environment by its GUID");
  g_tests.check(world.sky_light_count() == 1U, "the loaded scene has it");
  const engine::runtime::Entity entity = world.sky_light_entity_at(0U);

  g_tests.check(pipeline.execute_frame(), "a frame binds it");
  const engine::renderer::TextureHandle first = get_skybox_texture();
  g_tests.check((first != none) && engine::renderer::is_texture_cubemap(first),
                "the named map is the renderer's environment cubemap");
  engine::runtime::SkyLightComponent bound{};
  g_tests.check(world.get_sky_light_component(entity, &bound) &&
                    (bound.environmentAssetId ==
                     engine::content::make_asset_id_from_path(kSkyPath)),
                "the sky light shows the asset it resolved to");

  bool steady = true;
  for (int frame = 0; frame < 5; ++frame) {
    steady = steady && pipeline.execute_frame() &&
             (get_skybox_texture() == first);
  }
  g_tests.check(steady, "the map loads once, not every frame");

  g_tests.check(name_environment(pipeline, world, entity, kDuskGuid),
                "name another map");
  const engine::renderer::TextureHandle second = get_skybox_texture();
  g_tests.check((second != none) && (second != first) && live(second),
                "the other map replaces it");
  g_tests.check(!live(first), "the replaced map is released");
  g_tests.check(world.get_sky_light_component(entity, &bound) &&
                    (bound.environmentAssetId ==
                     engine::content::make_asset_id_from_path(kDuskPath)),
                "the sky light shows the new asset");

  g_tests.check(name_environment(pipeline, world, entity, kTextureGuid),
                "name a texture");
  g_tests.check((get_skybox_texture() == none) && !live(second),
                "a texture is not an environment: none, and the old map "
                "released");

  g_tests.check(name_environment(pipeline, world, entity, kSkyGuid),
                "name the first map again");
  const engine::renderer::TextureHandle third = get_skybox_texture();
  g_tests.check((third != none) && live(third), "it binds again");
  g_tests.check(world.remove_sky_light_component(entity) &&
                    pipeline.execute_frame(),
                "remove the sky light");
  g_tests.check((get_skybox_texture() == none) && !live(third),
                "no sky light: no environment, and the map released");
}

} // namespace

int main() {
  if (!write_project()) {
    g_tests.fail("write the project");
    return g_tests.finish("scene environment tests");
  }
  engine::EngineConfig config{};
  config.core.platform.headless = true;
  config.assetMount = "assets";
  config.assetRoot = kProject;
  config.mainScriptPath = "scene_environment_missing_main.lua";
  if (!engine::bootstrap(config)) {
    g_tests.fail("bootstrap");
  } else {
    {
      engine::EnginePipeline pipeline;
      engine::runtime::World *world = nullptr;
      if (!pipeline.initialize(0U) ||
          ((world = pipeline.world()) == nullptr)) {
        g_tests.fail("initialize the pipeline");
      } else {
        run_checks(pipeline, *world);
      }
      pipeline.teardown();
    }
    engine::shutdown();
  }
  std::error_code ec{};
  std::filesystem::remove_all(kProject, ec);
  return g_tests.finish("scene environment tests");
}
