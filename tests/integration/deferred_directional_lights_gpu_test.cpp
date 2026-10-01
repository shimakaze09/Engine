// Verifies on a real render device that the deferred path shades every
// directional light the forward path does. Deferred lighting used to read
// only the first sun, so a second sun lit forward-shaded surfaces (toon,
// unlit, transparent) and left the deferred props beside them dark, and a
// scene's look changed with r_deferred.
//
// A rough white physically-based cube faces the camera. The first sun
// shines straight down, onto its top, so its front face gets none of it;
// a second sun shines straight at the front face. The test reads the front
// face with the first sun alone, then with both, on the deferred path, and
// once more with both on the forward path: the second sun must brighten
// the deferred face, and to what the forward path shows.

#include "../gpu_scene_fixture.h"

#include "engine/content/asset_identity.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kMaterialPath = "assets/materials/two_suns_white.mat";
constexpr const char *kMaterialGuid = "c4d6f791-b21d-45e9-8fae-e6879c14b53a";

// The second sun lights the face by more than a quarter of the range a
// display shows, so a deferred face it never reached cannot pass.
constexpr double kMinBrighter = 64.0;
// Both paths shade the same surface with the same BRDF terms; what they
// may disagree by is the 8-bit G-buffer albedo against the forward
// path's float uniform, about one code value, and rounding in the two
// tonemap passes.
constexpr double kPathTolerance = 4.0;

bool write_text(const std::string &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  file.close();
  return !file.fail();
}

bool write_project_files() {
  std::error_code ec{};
  std::filesystem::create_directories("assets/materials", ec);
  return !ec &&
         write_text(std::string(kMaterialPath) + ".meta",
                    std::string("{\"schemaVersion\": 1, \"guid\": \"") +
                        kMaterialGuid + "\"}\n") &&
         write_text(kMaterialPath,
                    "{\"version\":4,\"shadingModel\":\"pbr\","
                    "\"albedo\":[1,1,1],\"metallic\":0,\"roughness\":1}");
}

void remove_project_files() {
  std::error_code ec{};
  std::filesystem::remove(kMaterialPath, ec);
  std::filesystem::remove(std::string(kMaterialPath) + ".meta", ec);
}

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

bool add_sun(World &world, const engine::math::Vec3 &direction) {
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent light{};
  light.direction = direction;
  light.intensity = 2.0F;
  return (sun != kInvalidEntity) && world.add_light_component(sun, light);
}

/// The front face's level in the next settled frame, or a negative value
/// when the device returned no readback.
double front_face(engine::EnginePipeline &pipeline, const char *name) {
  CapturedFrame frame{};
  if (!engine::tests::settle_frames(pipeline, 20) ||
      !engine::tests::capture_presented_frame(pipeline, name, &frame)) {
    return -1.0;
  }
  return block_level(frame, static_cast<int>(frame.width / 2U),
                     static_cast<int>(frame.height / 2U), 6);
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::checked;

  checked(engine::core::cvar_set_bool("r_deferred", true), "r_deferred");
  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_height_fog", false), "r_height_fog");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");
  checked(engine::core::cvar_set_bool("r_auto_exposure", false),
          "r_auto_exposure");

  engine::runtime::Transform transform{};
  transform.position = engine::math::Vec3(0.0F, 0.0F, -3.0F);
  const Entity cube = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = engine::content::make_asset_id_from_path("builtin://cube");
  mesh.materialAssetId =
      engine::content::make_asset_id_from_path(kMaterialPath);
  // Nearly straight down, onto the top face; the small tilt keeps the
  // shadow basis well defined and leaves the front face unlit by it.
  if ((cube == kInvalidEntity) || !world.add_mesh_component(cube, mesh) ||
      !add_sun(world, engine::math::Vec3(0.02F, -1.0F, -0.02F)) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }

  const double oneSun = front_face(pipeline, "two_suns_one.tga");
  if (oneSun < 0.0) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  // Shining along -Z, straight onto the face the camera sees.
  if (!add_sun(world, engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 11;
  }
  const double deferredTwo = front_face(pipeline, "two_suns_deferred.tga");
  checked(engine::core::cvar_set_bool("r_deferred", false), "r_deferred");
  const double forwardTwo = front_face(pipeline, "two_suns_forward.tga");
  std::printf("front face: one sun %.1f, two suns deferred %.1f, two suns "
              "forward %.1f\n",
              oneSun, deferredTwo, forwardTwo);
  if ((deferredTwo < 0.0) || (forwardTwo < 0.0)) {
    return 12;
  }
  if (forwardTwo < oneSun + kMinBrighter) {
    std::fprintf(stderr, "FAIL: the second sun does not light the face on "
                         "the forward path either; the scene is wrong\n");
    return 13;
  }
  if (deferredTwo < oneSun + kMinBrighter) {
    std::fprintf(stderr,
                 "FAIL: the deferred path ignores the second sun (%.1f "
                 "against %.1f with one)\n",
                 deferredTwo, oneSun);
    return 14;
  }
  if (std::fabs(deferredTwo - forwardTwo) > kPathTolerance) {
    std::fprintf(stderr,
                 "FAIL: deferred and forward shade the two suns differently "
                 "(%.1f against %.1f)\n",
                 deferredTwo, forwardTwo);
    return 15;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root() || !write_project_files()) {
    std::fprintf(stderr, "FAIL: could not write the test material\n");
    remove_project_files();
    return 1;
  }
  const int result = engine::tests::run_gpu_scene_test(
      "deferred_directional_lights_gpu_test", &run);
  remove_project_files();
  return result;
}
