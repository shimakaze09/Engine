// Verifies on a real render device that the deferred path keeps emission
// above 1: the G-buffer held emission in an 8-bit target, so a material
// authored to glow at 4 clamped to 1 on every deferred surface, lost its
// bloom, and looked the same as one authored at 1, while the forward path,
// whose scene target is a float one, kept it.
//
// Two black, rough PBR cubes side by side, drawn deferred with bloom and
// auto-exposure off: the left one's emission is 1, the right one's 4. Both
// pass through the same tonemap, which maps 4 well above 1 (ACES: 0.97
// against 0.80), so the right face is clearly the brighter; clamped at 1,
// the two faces are one colour. The materials and their sidecars are
// written into the asset directory before bootstrap, where the materials
// folder loads them.

#include "../gpu_scene_fixture.h"

#include "engine/content/asset_identity.h"

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

constexpr const char *kDimPath = "assets/materials/emissive_hdr_test_1.mat";
constexpr const char *kBrightPath = "assets/materials/emissive_hdr_test_4.mat";
constexpr const char *kDimGuid = "7f51a24c-6dc8-4094-9e5a-91324dcf6085";
constexpr const char *kBrightGuid = "8062b35d-7ed9-41a5-8f6b-a2435ed07196";

// Tonemapped ACES code values of emission 1 and 4 sit about 18 apart
// after sRGB encoding; the check asks for less than half of that, which
// no clamped target reaches (clamped, the faces are equal).
constexpr double kMinBrighter = 8.0;

bool write_text(const std::string &path, const std::string &text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
  file.close();
  return !file.fail();
}

std::string meta(const char *guid) {
  return std::string("{\"schemaVersion\": 1, \"guid\": \"") + guid + "\"}\n";
}

std::string material(const char *emission) {
  return std::string("{\"version\":4,\"shadingModel\":\"pbr\","
                     "\"albedo\":[0,0,0],\"metallic\":0,\"roughness\":1,"
                     "\"emissive\":[") +
         emission + "," + emission + "," + emission + "]}";
}

bool write_project_files() {
  std::error_code ec{};
  std::filesystem::create_directories("assets/materials", ec);
  return !ec && write_text(std::string(kDimPath) + ".meta", meta(kDimGuid)) &&
         write_text(std::string(kBrightPath) + ".meta", meta(kBrightGuid)) &&
         write_text(kDimPath, material("1")) &&
         write_text(kBrightPath, material("4"));
}

void remove_project_files() {
  std::error_code ec{};
  for (const char *path : {kDimPath, kBrightPath}) {
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::string(path) + ".meta", ec);
  }
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

Entity add_cube(World &world, const engine::math::Vec3 &position,
                const char *materialPath) {
  engine::runtime::Transform transform{};
  transform.position = position;
  const Entity cube = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = engine::content::make_asset_id_from_path("builtin://cube");
  mesh.materialAssetId = engine::content::make_asset_id_from_path(materialPath);
  return ((cube != kInvalidEntity) && world.add_mesh_component(cube, mesh))
             ? cube
             : kInvalidEntity;
}

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::checked;
  using engine::tests::settle_frames;

  checked(engine::core::cvar_set_bool("r_deferred", true), "r_deferred");
  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_height_fog", false), "r_height_fog");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");
  checked(engine::core::cvar_set_bool("r_auto_exposure", false),
          "r_auto_exposure");
  checked(engine::core::cvar_set_float("r_exposure", 1.0F), "r_exposure");
  checked(engine::core::cvar_set_int("r_tonemap_operator", 1),
          "r_tonemap_operator");

  if ((add_cube(world, engine::math::Vec3(-1.2F, 0.0F, -4.0F), kDimPath) ==
       kInvalidEntity) ||
      (add_cube(world, engine::math::Vec3(1.2F, 0.0F, -4.0F), kBrightPath) ==
       kInvalidEntity) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }

  CapturedFrame frame{};
  if (!settle_frames(pipeline, 30) ||
      !capture_presented_frame(pipeline, "deferred_emissive_hdr.tga", &frame)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const int cy = static_cast<int>(frame.height / 2U);
  const double dim =
      block_level(frame, static_cast<int>((frame.width * 1U) / 3U), cy, 6);
  const double bright =
      block_level(frame, static_cast<int>((frame.width * 2U) / 3U), cy, 6);
  std::printf("front faces: emission 1 -> %.1f, emission 4 -> %.1f\n", dim,
              bright);
  if (dim < 128.0) {
    std::fprintf(stderr, "FAIL: the emission-1 face did not glow (%.1f)\n",
                 dim);
    return 11;
  }
  if (bright < dim + kMinBrighter) {
    std::fprintf(stderr,
                 "FAIL: emission 4 renders no brighter than emission 1 "
                 "(%.1f against %.1f); the deferred path clamps it\n",
                 bright, dim);
    return 12;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::tests::enter_asset_root() || !write_project_files()) {
    std::fprintf(stderr, "FAIL: could not write the test materials\n");
    remove_project_files();
    return 1;
  }
  const int result =
      engine::tests::run_gpu_scene_test("deferred_emissive_hdr_gpu_test", &run);
  remove_project_files();
  return result;
}
