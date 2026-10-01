// Verifies on a real render device that a base-colour texture is read as
// the sRGB image it is (issue #810): the renderer used to upload colour
// textures as plain RGBA8 and light their encoded bytes as though they were
// linear, so every textured surface came out lighter than authored.
//
// Two unlit cubes side by side. The left one's material takes its base
// colour from a uniform texture of sRGB code 128; the right one's is the
// constant linear colour that code stands for, 0.2158605. Read as sRGB, the
// texture and the constant are one colour, so the two faces match; read as
// linear, the textured face is 0.502 and far brighter. The materials, the
// texture and their sidecars are written into the asset directory before
// bootstrap, where the mount walk and the materials folder load them.

#include "../gpu_scene_fixture.h"
#include "../png_fixture.h"

#include "engine/content/asset_identity.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kTexturePath = "assets/textures/srgb_test_grey.png";
constexpr const char *kTexturedPath = "assets/materials/srgb_test_textured.mat";
constexpr const char *kConstantPath = "assets/materials/srgb_test_constant.mat";
constexpr const char *kTextureGuid = "4c2e7f10-3a95-4d61-8b27-6e0f1a9c3d52";
constexpr const char *kTexturedGuid = "5d3f8021-4ba6-4e72-9c38-7f102bad4e63";
constexpr const char *kConstantGuid = "6e40913b-5cb7-4f83-8d49-80213cbe5f74";

bool write_bytes(const char *path, const void *bytes, std::size_t size) {
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

bool write_text(const std::string &path, const std::string &text) {
  return write_bytes(path.c_str(), text.data(), text.size());
}

/// A 64x64 opaque RGB PNG whose every channel is `level`.
std::vector<unsigned char> grey_png(unsigned char level) {
  return engine::tests::uniform_rgb8_png(64U, level);
}

std::string meta(const char *guid) {
  return std::string("{\"schemaVersion\": 1, \"guid\": \"") + guid + "\"}\n";
}

bool write_project_files() {
  std::error_code ec{};
  std::filesystem::create_directories("assets/textures", ec);
  std::filesystem::create_directories("assets/materials", ec);
  const std::vector<unsigned char> png = grey_png(128U);
  return !ec && write_bytes(kTexturePath, png.data(), png.size()) &&
         write_text(std::string(kTexturePath) + ".meta", meta(kTextureGuid)) &&
         write_text(std::string(kTexturedPath) + ".meta",
                    meta(kTexturedGuid)) &&
         write_text(std::string(kConstantPath) + ".meta",
                    meta(kConstantGuid)) &&
         write_text(kTexturedPath,
                    std::string("{\"version\":4,\"shadingModel\":\"unlit\","
                                "\"albedo\":[1,1,1],\"textures\":"
                                "{\"albedo\":\"") +
                        kTextureGuid + "\"}}") &&
         write_text(kConstantPath,
                    "{\"version\":4,\"shadingModel\":\"unlit\","
                    "\"albedo\":[0.2158605,0.2158605,0.2158605]}");
}

void remove_project_files() {
  std::error_code ec{};
  for (const char *path : {kTexturePath, kTexturedPath, kConstantPath}) {
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::string(path) + ".meta", ec);
  }
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

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");

  // The camera looks down -Z from the origin at two cubes 4 units away,
  // one left of centre and one right, whose front faces it sees square on.
  if ((add_cube(world, engine::math::Vec3(-1.2F, 0.0F, -4.0F), kTexturedPath) ==
       kInvalidEntity) ||
      (add_cube(world, engine::math::Vec3(1.2F, 0.0F, -4.0F), kConstantPath) ==
       kInvalidEntity) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }

  CapturedFrame frame{};
  if (!settle_frames(pipeline, 30) ||
      !capture_presented_frame(pipeline, "srgb_texture.tga", &frame)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const int cy = static_cast<int>(frame.height / 2U);
  const double textured =
      block_level(frame, static_cast<int>((frame.width * 1U) / 3U), cy, 6);
  const double constant =
      block_level(frame, static_cast<int>((frame.width * 2U) / 3U), cy, 6);
  std::printf("front faces: textured %.1f, constant %.1f\n", textured,
              constant);
  if (constant < 16.0) {
    std::fprintf(stderr, "FAIL: the constant face did not draw (%.1f)\n",
                 constant);
    return 11;
  }
  // One colour through two routes, the same shading and exposure: the
  // faces match to within rounding, two levels of the 8-bit readback (the
  // texel is quantised to code 128, the constant is not).
  if (std::fabs(textured - constant) > 2.0) {
    std::fprintf(stderr,
                 "FAIL: the sRGB texture does not read as the colour it "
                 "encodes (%.1f against %.1f)\n",
                 textured, constant);
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
      engine::tests::run_gpu_scene_test("srgb_texture_gpu_test", &run);
  remove_project_files();
  return result;
}
