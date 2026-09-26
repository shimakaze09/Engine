// Verifies on a real render device that a scene's sky light lights the
// scene (issue #580): a white diffuse sphere with no direct light, under
// the default procedural sky, is drawn once with no sky light, once with a
// sky light naming a uniform white environment map, and once after the
// sky light is removed. The environment is found the way a project's
// would be: a Radiance file with a sidecar in the asset directory,
// catalogued by the mount walk at bootstrap and named by its GUID.
//
// Under a uniform environment of radiance 1 the diffuse irradiance is pi,
// so a white Lambertian surface leaves with radiance 1 before exposure;
// without an environment only the renderer's small constant ambient
// reaches it. So the lit centre must be far brighter than the unlit one,
// and removing the sky light must bring back the unlit frame.

#include "../gpu_scene_fixture.h"

#include "engine/content/asset_identity.h"
#include "engine/renderer/command_buffer.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kEnvironmentPath =
    "assets/scene_environment_gpu_test_white.hdr";
constexpr const char *kMetaPath =
    "assets/scene_environment_gpu_test_white.hdr.meta";
constexpr const char *kEnvironmentGuid = "6f9b4c5d-be70-41a2-8d4f-8a5b1c7e9d34";

/// Writes `text` to `path`; false when any step fails.
bool write_text(const char *path, const void *bytes, std::size_t size) {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  const bool wrote = std::fwrite(bytes, 1U, size, file) == size;
  return (std::fclose(file) == 0) && wrote;
}

/// An 8x4 flat Radiance picture of constant white (RGBE 128,128,128,129
/// is 1.0 per channel) and the sidecar that gives it its identity.
bool write_environment() noexcept {
  unsigned char picture[64 + (32 * 4)] = {};
  const char header[] = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n";
  const std::size_t headerSize = sizeof(header) - 1U;
  for (std::size_t i = 0U; i < headerSize; ++i) {
    picture[i] = static_cast<unsigned char>(header[i]);
  }
  for (std::size_t pixel = 0U; pixel < 32U; ++pixel) {
    unsigned char *rgbe = picture + headerSize + (pixel * 4U);
    rgbe[0] = 128U;
    rgbe[1] = 128U;
    rgbe[2] = 128U;
    rgbe[3] = 129U;
  }
  char meta[128] = {};
  const int metaSize =
      std::snprintf(meta, sizeof(meta),
                    "{\"schemaVersion\": 1, \"guid\": \"%s\"}\n",
                    kEnvironmentGuid);
  return write_text(kEnvironmentPath, picture, headerSize + (32U * 4U)) &&
         (metaSize > 0) &&
         write_text(kMetaPath, meta, static_cast<std::size_t>(metaSize));
}

void remove_environment() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kEnvironmentPath, ec);
  std::filesystem::remove(kMetaPath, ec);
}

/// Mean brightness of the 9x9 block at the frame's centre.
double centre_level(const CapturedFrame &frame) noexcept {
  const int cx = static_cast<int>(frame.width / 2U);
  const int cy = static_cast<int>(frame.height / 2U);
  double sum = 0.0;
  int count = 0;
  for (int y = cy - 4; y <= cy + 4; ++y) {
    for (int x = cx - 4; x <= cx + 4; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        sum += frame.channel(static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y), c);
        ++count;
      }
    }
  }
  return sum / static_cast<double>(count);
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::checked;
  using engine::tests::settle_frames;

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");

  engine::runtime::Transform transform{};
  transform.scale = engine::math::Vec3(6.0F, 6.0F, 6.0F);
  const Entity sphere = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId =
      engine::content::make_asset_id_from_path("builtin://sphere");
  mesh.albedo = engine::math::Vec3(1.0F, 1.0F, 1.0F);
  mesh.metallic = 0.0F;
  mesh.roughness = 1.0F;
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.0F;
  if ((sphere == kInvalidEntity) || !world.add_mesh_component(sphere, mesh) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 9.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  CapturedFrame unlit{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "scene_env_unlit.tga", &unlit)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  engine::runtime::SkyLightComponent skyLight{};
  const Entity sky = world.create_scene_object();
  if (!engine::content::parse_asset_ref(kEnvironmentGuid,
                                        &skyLight.environmentRef) ||
      (sky == kInvalidEntity) ||
      !world.add_sky_light_component(sky, skyLight)) {
    return 11;
  }
  CapturedFrame lit{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "scene_env_lit.tga", &lit)) {
    return 12;
  }
  if (engine::renderer::get_skybox_texture() ==
      engine::renderer::kInvalidTextureHandle) {
    std::fprintf(stderr, "FAIL: the sky light set no environment\n");
    return 13;
  }

  if (!world.remove_sky_light_component(sky)) {
    return 14;
  }
  CapturedFrame restored{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "scene_env_restored.tga",
                               &restored)) {
    return 15;
  }

  const double unlitLevel = centre_level(unlit);
  const double litLevel = centre_level(lit);
  const double restoredLevel = centre_level(restored);
  std::printf("centre: unlit %.1f, lit %.1f, restored %.1f\n", unlitLevel,
              litLevel, restoredLevel);
  // A lit centre leaves with radiance 1, which the tonemap puts well into
  // the upper half of the range; the unlit one sees only the constant
  // ambient. Sixty-four levels, a quarter of the range, is far below that
  // gap and far above any frame-to-frame difference of one scene.
  if (litLevel < unlitLevel + 64.0) {
    std::fprintf(stderr,
                 "FAIL: the environment did not light the sphere "
                 "(%.1f against %.1f unlit)\n",
                 litLevel, unlitLevel);
    return 16;
  }
  // The same scene twice with nothing in between that varies: the frames
  // match to within one level of rounding.
  if (std::fabs(restoredLevel - unlitLevel) > 1.0) {
    std::fprintf(stderr,
                 "FAIL: removing the sky light did not restore the scene "
                 "(%.1f against %.1f)\n",
                 restoredLevel, unlitLevel);
    return 17;
  }
  return 0;
}

} // namespace

int main() {
  // The mount walk at bootstrap is what catalogues a project's assets, so
  // the environment map is in the asset directory before the engine starts.
  if (!engine::tests::detail::enter_asset_directory() || !write_environment()) {
    std::fprintf(stderr, "FAIL: could not write the environment map\n");
    remove_environment();
    return 1;
  }
  const int result = engine::tests::run_gpu_scene_test(
      "scene_environment_gpu_test", &run);
  remove_environment();
  return result;
}
