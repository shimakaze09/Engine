// Verifies on a real render device that SSAO darkens contact creases and
// leaves open surfaces alone: a unit cube stands on a white floor under a
// uniform white environment (a sky light and no sun, so the ambient term
// SSAO scales is the whole of the lighting). The frame is captured with
// r_ssao off and on, and the darkening SSAO adds is measured at three
// points projected from the scene: the open floor, well away from the
// cube; the cube's open front face, well above the floor; and the floor
// just in front of the face, in the crease. The open points must read as
// SSAO-off does and the crease measurably darker. On the HLSL-family
// shader profiles (spirv, dx, metal) the kernel's basis was built as rows,
// turning the sample hemisphere away from the normal, and the open floor
// darkened more than half as much as the crease.

#include "../gpu_scene_fixture.h"

#include "engine/content/asset_identity.h"
#include "engine/math/transform.h"
#include "engine/renderer/camera.h"
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

constexpr const char *kEnvironmentPath = "assets/ssao_contact_gpu_white.hdr";
constexpr const char *kMetaPath = "assets/ssao_contact_gpu_white.hdr.meta";
constexpr const char *kEnvironmentGuid = "4b6e9a12-3c5d-47f8-9e21-0a7c5d3f8b16";

bool write_file(const char *path, const void *bytes, std::size_t size) {
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

/// An 8x4 flat Radiance picture of constant radiance 1 and its sidecar.
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
  const int metaSize = std::snprintf(
      meta, sizeof(meta), "{\"schemaVersion\": 1, \"guid\": \"%s\"}\n",
      kEnvironmentGuid);
  return write_file(kEnvironmentPath, picture, headerSize + (32U * 4U)) &&
         (metaSize > 0) &&
         write_file(kMetaPath, meta, static_cast<std::size_t>(metaSize));
}

void remove_environment() noexcept {
  std::error_code ec{};
  std::filesystem::remove(kEnvironmentPath, ec);
  std::filesystem::remove(kMetaPath, ec);
}

/// Mean brightness of the (2r+1)^2 block centred on (cx, cy).
double block_level(const CapturedFrame &frame, int cx, int cy, int r) noexcept {
  double sum = 0.0;
  int count = 0;
  for (int y = cy - r; y <= cy + r; ++y) {
    for (int x = cx - r; x <= cx + r; ++x) {
      for (std::uint32_t c = 0U; c < 3U; ++c) {
        sum += frame.channel(static_cast<std::uint32_t>(x),
                             static_cast<std::uint32_t>(y), c);
        ++count;
      }
    }
  }
  return sum / static_cast<double>(count);
}

/// Pixel a world point lands on under the active camera.
bool project(const CapturedFrame &frame, const engine::math::Vec3 &point,
             int *outX, int *outY) noexcept {
  const engine::renderer::CameraState camera =
      engine::renderer::get_active_camera();
  const float aspect =
      static_cast<float>(frame.width) / static_cast<float>(frame.height);
  const engine::math::Mat4 viewProjection = engine::math::mul(
      engine::renderer::camera_projection_matrix(camera, aspect),
      engine::math::look_at(camera.position, camera.target, camera.up));
  const engine::math::Vec4 clip = engine::math::mul(
      viewProjection, engine::math::Vec4(point.x, point.y, point.z, 1.0F));
  if (!(clip.w > 0.0F)) {
    return false;
  }
  const float ndcX = clip.x / clip.w;
  const float ndcY = clip.y / clip.w;
  *outX =
      static_cast<int>((ndcX * 0.5F + 0.5F) * static_cast<float>(frame.width));
  *outY =
      static_cast<int>((0.5F - ndcY * 0.5F) * static_cast<float>(frame.height));
  return (*outX > 8) && (*outY > 8) &&
         (*outX < static_cast<int>(frame.width) - 8) &&
         (*outY < static_cast<int>(frame.height) - 8);
}

struct Region final {
  const char *name;
  engine::math::Vec3 point;
  double darkening = 0.0;
};

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::checked;
  using engine::tests::settle_frames;

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_height_fog", false), "r_height_fog");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");

  engine::runtime::Transform floorTransform{};
  engine::runtime::Transform cubeTransform{};
  cubeTransform.position = engine::math::Vec3(0.0F, 0.5F, 0.0F);
  const Entity floor =
      engine::tests::add_builtin_mesh(world, "builtin://plane", floorTransform,
                                      engine::math::Vec3(1.0F, 1.0F, 1.0F));
  const Entity cube =
      engine::tests::add_builtin_mesh(world, "builtin://cube", cubeTransform,
                                      engine::math::Vec3(1.0F, 1.0F, 1.0F));
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.intensity = 0.0F;
  engine::runtime::SkyLightComponent skyLight{};
  const Entity sky = world.create_scene_object();
  if ((floor == kInvalidEntity) || (cube == kInvalidEntity) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::content::parse_asset_ref(kEnvironmentGuid,
                                        &skyLight.environmentRef) ||
      (sky == kInvalidEntity) ||
      !world.add_sky_light_component(sky, skyLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.6F, 1.6F, 3.2F),
                                engine::math::Vec3(0.0F, 0.4F, 0.0F))) {
    return 10;
  }

  CapturedFrame off{};
  CapturedFrame on{};
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "ssao_contact_off.tga", &off)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  checked(engine::core::cvar_set_bool("r_ssao", true), "r_ssao");
  if (!settle_frames(pipeline, 20) ||
      !capture_presented_frame(pipeline, "ssao_contact_on.tga", &on)) {
    return 11;
  }

  Region regions[] = {
      {"open floor", engine::math::Vec3(-1.4F, 0.0F, 1.2F)},
      {"open face", engine::math::Vec3(0.0F, 0.8F, 0.5F)},
      {"crease", engine::math::Vec3(0.0F, 0.0F, 0.56F)},
  };
  for (Region &region : regions) {
    int x = 0;
    int y = 0;
    if (!project(on, region.point, &x, &y)) {
      std::fprintf(stderr, "FAIL: %s is off the frame\n", region.name);
      return 12;
    }
    region.darkening = block_level(off, x, y, 2) - block_level(on, x, y, 2);
    std::printf("%s at (%d, %d): off %.2f, on %.2f, darkening %.2f\n",
                region.name, x, y, block_level(off, x, y, 2),
                block_level(on, x, y, 2), region.darkening);
  }
  // Nothing occludes an open point within the kernel radius, so SSAO must
  // leave it as it was: one level covers readback rounding.
  for (int i = 0; i < 2; ++i) {
    if (std::fabs(regions[i].darkening) > 1.0) {
      std::fprintf(stderr, "FAIL: SSAO darkened the %s by %.2f levels\n",
                   regions[i].name, regions[i].darkening);
      return 13;
    }
  }
  // The crease is half enclosed by the cube's face. Four levels is well
  // clear of the open points' rounding and under the darkening the
  // kernel produces there (6.8 on lavapipe).
  if (regions[2].darkening < 4.0) {
    std::fprintf(stderr, "FAIL: SSAO darkened the crease by only %.2f levels\n",
                 regions[2].darkening);
    return 14;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::tests::detail::enter_asset_directory() || !write_environment()) {
    std::fprintf(stderr, "FAIL: could not write the environment map\n");
    remove_environment();
    return 1;
  }
  const int result =
      engine::tests::run_gpu_scene_test("ssao_contact_gpu_test", &run);
  remove_environment();
  return result;
}
