// Verifies on a real render device that a material whose Alpha Mode is
// Blend composites with what is behind it by its opacity texture. The
// mode was offered and saved, but the transparent classification read only
// the scalar opacity and the shader's alpha ignored the texture, so a
// soft-edged Blend card at opacity 1 drew fully opaque.
//
// A large white unlit backdrop fills the view. In front of its left half
// hangs a black unlit cube whose material is Blend at opacity 1 with a
// uniform 50% opacity texture. Composited, the cube's face shows half the
// backdrop: well above black, well below the bare backdrop on the right.
// The materials, the texture and their sidecars are written into the asset
// directory before bootstrap, where the mount walk and the materials folder
// load them.

#include "../gpu_scene_fixture.h"
#include "../png_fixture.h"

#include "engine/content/asset_identity.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kTexturePath = "assets/textures/alpha_blend_half.png";
constexpr const char *kCardPath = "assets/materials/alpha_blend_card.mat";
constexpr const char *kBackdropPath =
    "assets/materials/alpha_blend_backdrop.mat";
constexpr const char *kTextureGuid = "91a3c46e-8fea-42b6-9e7b-b3546fe18207";
constexpr const char *kCardGuid = "a2b4d57f-90fb-43c7-8f8c-c4657af29318";
constexpr const char *kBackdropGuid = "b3c5e680-a10c-44d8-9f9d-d5768b03a429";

bool write_bytes(const std::string &path,
                 const std::vector<unsigned char> &bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  file.close();
  return !file.fail();
}

bool write_text(const std::string &path, const std::string &text) {
  return write_bytes(path, std::vector<unsigned char>(text.begin(), text.end()));
}

std::string meta(const char *guid) {
  return std::string("{\"schemaVersion\": 1, \"guid\": \"") + guid + "\"}\n";
}

bool write_project_files() {
  std::error_code ec{};
  std::filesystem::create_directories("assets/textures", ec);
  std::filesystem::create_directories("assets/materials", ec);
  return !ec &&
         write_bytes(kTexturePath, engine::tests::uniform_rgb8_png(16U, 128U)) &&
         write_text(std::string(kTexturePath) + ".meta", meta(kTextureGuid)) &&
         write_text(std::string(kCardPath) + ".meta", meta(kCardGuid)) &&
         write_text(std::string(kBackdropPath) + ".meta",
                    meta(kBackdropGuid)) &&
         write_text(kCardPath,
                    std::string("{\"version\":4,\"shadingModel\":\"unlit\","
                                "\"albedo\":[0,0,0],\"alphaMode\":\"blend\","
                                "\"textures\":{\"opacity\":\"") +
                        kTextureGuid + "\"}}") &&
         write_text(kBackdropPath, "{\"version\":4,\"shadingModel\":\"unlit\","
                                   "\"albedo\":[1,1,1]}");
}

void remove_project_files() {
  std::error_code ec{};
  for (const char *path : {kTexturePath, kCardPath, kBackdropPath}) {
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

Entity add_cube(World &world, const engine::math::Vec3 &position, float scale,
                const char *materialPath) {
  engine::runtime::Transform transform{};
  transform.position = position;
  transform.scale = engine::math::Vec3(scale, scale, scale);
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
  checked(engine::core::cvar_set_bool("r_height_fog", false), "r_height_fog");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");
  checked(engine::core::cvar_set_bool("r_auto_exposure", false),
          "r_auto_exposure");

  if ((add_cube(world, engine::math::Vec3(0.0F, 0.0F, -12.0F), 12.0F,
                kBackdropPath) == kInvalidEntity) ||
      (add_cube(world, engine::math::Vec3(-1.2F, 0.0F, -4.0F), 1.0F,
                kCardPath) == kInvalidEntity) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }

  CapturedFrame frame{};
  if (!settle_frames(pipeline, 30) ||
      !capture_presented_frame(pipeline, "alpha_blend.tga", &frame)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const int cy = static_cast<int>(frame.height / 2U);
  const double card =
      block_level(frame, static_cast<int>((frame.width * 1U) / 3U), cy, 6);
  const double backdrop =
      block_level(frame, static_cast<int>((frame.width * 2U) / 3U), cy, 6);
  std::printf("blend card %.1f over backdrop %.1f\n", card, backdrop);
  if (backdrop < 128.0) {
    std::fprintf(stderr, "FAIL: the backdrop did not draw (%.1f)\n",
                 backdrop);
    return 11;
  }
  // Half the backdrop's linear light, through the tonemap: far from both
  // black (an opaque card) and the bare backdrop (an invisible one).
  if ((card < 0.25 * backdrop) || (card > 0.95 * backdrop)) {
    std::fprintf(stderr,
                 "FAIL: the Blend card does not composite by its opacity "
                 "texture (%.1f against a backdrop of %.1f)\n",
                 card, backdrop);
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
      engine::tests::run_gpu_scene_test("alpha_blend_gpu_test", &run);
  remove_project_files();
  return result;
}
