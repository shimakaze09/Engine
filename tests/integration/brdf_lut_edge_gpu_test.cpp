// GPU regression for issue #565 row 1, the BRDF integration table: it
// clamps at its edges. The split-sum ambient term looks the table up at
// (NdotV, roughness) with linear filtering, so the point of a surface that
// faces the viewer squarely reads it at u = 1.0 exactly. With a repeating
// wrap that tap sits on the seam and blends the last column with the first
// — the head-on BRDF with the grazing one. For a dielectric those differ by
// an order of magnitude (head-on reflects F0, about 4%; grazing reflects
// nearly everything), so every smooth object showed a bright pinpoint where
// it faced the camera. Nothing on the CPU side sees what a tap on the seam
// returns.
//
// One large, smooth, black dielectric sphere on the view axis under a
// uniform white environment and no direct light, so what reaches the eye
// is the environment times the table's value. The centre of its disc must
// be as dim as the ring just around it.
//
// No production code sets a cubemap sky, so image-based lighting is
// switched on here by hand: a small white Radiance file written into the
// build tree's asset copy, loaded through the production loader.

#include "../gpu_scene_fixture.h"

#include "engine/renderer/command_buffer.h"
#include "engine/renderer/texture_loader.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kEnvironmentPath = "assets/brdf_lut_edge_test_white.hdr";

/// Writes an 8x4 flat (not run-length) Radiance picture of constant white.
/// RGBE (128, 128, 128, 129) is 0.5 * 2^1 = 1.0 per channel, and a pixel
/// that does not start with the bytes 2, 2 is read as flat.
bool write_white_environment() noexcept {
  std::FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, kEnvironmentPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(kEnvironmentPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }
  bool ok = std::fputs("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 8\n",
                       file) >= 0;
  const unsigned char white[4] = {128U, 128U, 128U, 129U};
  for (int pixel = 0; ok && (pixel < 32); ++pixel) {
    ok = std::fwrite(white, 1U, sizeof(white), file) == sizeof(white);
  }
  return (std::fclose(file) == 0) && ok;
}

/// Mean brightness over the pixels whose distance from (cx, cy) lies in
/// [innerRadius, outerRadius).
double ring_level(const CapturedFrame &frame, int cx, int cy, int innerRadius,
                  int outerRadius) noexcept {
  std::uint64_t sum = 0U;
  std::uint64_t count = 0U;
  for (int y = cy - outerRadius; y <= cy + outerRadius; ++y) {
    for (int x = cx - outerRadius; x <= cx + outerRadius; ++x) {
      const int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
      if ((d2 < innerRadius * innerRadius) || (d2 >= outerRadius * outerRadius) ||
          (x < 0) || (y < 0) || (x >= static_cast<int>(frame.width)) ||
          (y >= static_cast<int>(frame.height))) {
        continue;
      }
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        sum += frame.channel(static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y), c);
        ++count;
      }
    }
  }
  return (count > 0U) ? static_cast<double>(sum) / static_cast<double>(count)
                      : 0.0;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::checked;
  using engine::tests::settle_frames;

  // The table is sampled by the forward program's full-PBR variant. Bloom
  // would smear a pinpoint into its surroundings and FXAA would soften it,
  // so neither runs.
  checked(engine::core::cvar_set_bool("r_deferred", false), "r_deferred");
  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");

  // Six metres across and nine away: its disc is about 440 pixels wide in a
  // 720-line frame, so the patch that faces the viewer to within the
  // table's last half texel (NdotV above 1 - 0.5/512, about 2.5 degrees) is
  // some ten pixels in radius.
  engine::runtime::Transform transform{};
  transform.scale = engine::math::Vec3(6.0F, 6.0F, 6.0F);
  const Entity sphere = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = engine::content::make_asset_id_from_path("builtin://sphere");
  mesh.albedo = engine::math::Vec3(0.0F, 0.0F, 0.0F);
  mesh.metallic = 0.0F;
  mesh.roughness = 0.04F;
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.0F;
  if ((sphere == kInvalidEntity) || !world.add_mesh_component(sphere, mesh) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 9.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  // Without an environment the ambient term is a constant and the table is
  // never read; this frame is the proof that the next one does read it.
  CapturedFrame withoutEnvironment{};
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "lut_no_env.tga",
                               &withoutEnvironment)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  if (!write_white_environment()) {
    return 12;
  }
  const engine::renderer::TextureHandle environment =
      engine::renderer::load_hdr_equirect_cubemap(kEnvironmentPath, 64);
  std::error_code ec{};
  std::filesystem::remove(kEnvironmentPath, ec);
  if (environment == engine::renderer::kInvalidTextureHandle) {
    std::fprintf(stderr, "FAIL: the white environment did not load\n");
    return 13;
  }
  engine::renderer::set_skybox_texture(environment);
  checked(engine::core::cvar_set_string("r_sky_model", "cubemap"),
          "r_sky_model");

  CapturedFrame frame{};
  const bool captured = settle_frames(pipeline, 20) &&
                        capture_presented_frame(pipeline, "lut_env.tga", &frame);
  engine::renderer::set_skybox_texture(engine::renderer::kInvalidTextureHandle);
  checked(engine::core::cvar_set_string("r_sky_model", "hosek"), "r_sky_model");
  if (!captured) {
    return 14;
  }

  const int cx = static_cast<int>(frame.width / 2U);
  const int cy = static_cast<int>(frame.height / 2U);
  const double centre = ring_level(frame, cx, cy, 0, 4);
  const double around = ring_level(frame, cx, cy, 24, 40);
  // Near the rim the view grazes the surface and Fresnel takes the
  // reflection toward the full environment.
  const double rim = ring_level(frame, cx, cy, 200, 212);
  const double rimWithout = ring_level(withoutEnvironment, cx, cy, 200, 212);
  std::printf("brdf_lut_edge_gpu_test: %ux%u centre %.2f, ring around it "
              "%.2f, near the rim %.2f (%.2f without an environment)\n",
              frame.width, frame.height, centre, around, rim, rimWithout);

  int result = 0;
  // The environment has to be what the sphere shows, or the table was never
  // read and the comparison below is between two constants.
  if ((rim - rimWithout) < 20.0) {
    std::fprintf(stderr, "FAIL: the environment changed the rim by only %.2f "
                         "levels; image-based lighting is not active\n",
                 rim - rimWithout);
    result = 20;
  }
  // Head-on reflectance changes by well under a percent across the few
  // degrees between the centre and that ring, so the two differ by no more
  // than shading noise; a seam tap lifts the centre by tens of levels. Two
  // levels is far from both.
  if ((centre - around) > 2.0) {
    std::fprintf(stderr, "FAIL: the point facing the viewer is %.2f levels "
                         "brighter than the surface around it; the table's "
                         "edge does not clamp\n",
                 centre - around);
    result = 21;
  }
  return result;
}

} // namespace

/// Runs this executable or test program.
int main() {
  return engine::tests::run_gpu_scene_test("brdf_lut_edge_gpu_test", &run);
}
