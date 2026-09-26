// Verifies on a real render device that a minified material texture samples
// its image (issue #709): the bgfx backend used to allocate a texture's
// generated mip chain and fill only level 0, so a surface small enough on
// screen to sample a lower level drew from empty memory.
//
// Two identical cubes carry one material whose albedo is a uniform white
// 256x256 texture. The near one spans some 180 pixels and samples level 0;
// the far one spans some 24 and samples level three or so. A directional light
// behind the camera lights both front faces alike, so with every level holding
// the image the two faces are the same colour. The material, its texture and
// their sidecars are written into the asset directory before bootstrap, where
// the mount walk and the materials folder load a project's own.

#include "../gpu_scene_fixture.h"

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

constexpr const char *kTexturePath = "assets/textures/mip_chain_test_white.png";
constexpr const char *kMaterialPath = "assets/materials/mip_chain_test.mat";
constexpr const char *kTextureGuid = "7a0c5d6e-cf81-42b3-9e5a-9b6c2d8f0e45";
constexpr const char *kMaterialGuid = "8b1d6e7f-d092-43c4-8f6b-0c7d3e9a1f56";

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

/// CRC-32 as PNG chunks use it.
std::uint32_t crc32(const std::vector<unsigned char> &bytes, std::size_t from) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = from; i < bytes.size(); ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

void put_u32(std::vector<unsigned char> *out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out->push_back(static_cast<unsigned char>((value >> shift) & 0xFFU));
  }
}

void put_chunk(std::vector<unsigned char> *png, const char *type,
               const std::vector<unsigned char> &data) {
  put_u32(png, static_cast<std::uint32_t>(data.size()));
  const std::size_t start = png->size();
  png->insert(png->end(), type, type + 4);
  png->insert(png->end(), data.begin(), data.end());
  std::vector<unsigned char> crcInput(
      png->begin() + static_cast<std::ptrdiff_t>(start), png->end());
  put_u32(png, crc32(crcInput, 0U));
}

/// A 256x256 opaque white RGB PNG, stored uncompressed (deflate "stored"
/// blocks), so the test needs no compressor.
std::vector<unsigned char> white_png() {
  constexpr std::uint32_t kSize = 256U;
  std::vector<unsigned char> raw{};
  for (std::uint32_t y = 0U; y < kSize; ++y) {
    raw.push_back(0U); // filter: none
    raw.insert(raw.end(), kSize * 3U, 255U);
  }
  std::vector<unsigned char> zlib{0x78U, 0x01U};
  std::uint32_t a = 1U;
  std::uint32_t b = 0U;
  for (const unsigned char byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  for (std::size_t offset = 0U; offset < raw.size(); offset += 65535U) {
    const std::size_t length =
        ((raw.size() - offset) < 65535U) ? (raw.size() - offset) : 65535U;
    const bool last = (offset + length) == raw.size();
    zlib.push_back(last ? 1U : 0U);
    zlib.push_back(static_cast<unsigned char>(length & 0xFFU));
    zlib.push_back(static_cast<unsigned char>((length >> 8U) & 0xFFU));
    zlib.push_back(static_cast<unsigned char>(~length & 0xFFU));
    zlib.push_back(static_cast<unsigned char>((~length >> 8U) & 0xFFU));
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
  }
  put_u32(&zlib, (b << 16U) | a);

  std::vector<unsigned char> png{0x89U, 'P',   'N',   'G',
                                 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  std::vector<unsigned char> header{};
  put_u32(&header, kSize);
  put_u32(&header, kSize);
  header.insert(header.end(), {8U, 2U, 0U, 0U, 0U}); // 8-bit RGB
  put_chunk(&png, "IHDR", header);
  put_chunk(&png, "IDAT", zlib);
  put_chunk(&png, "IEND", {});
  return png;
}

std::string meta(const char *guid) {
  return std::string("{\"schemaVersion\": 1, \"guid\": \"") + guid + "\"}\n";
}

bool write_project_files() {
  std::error_code ec{};
  std::filesystem::create_directories("assets/textures", ec);
  std::filesystem::create_directories("assets/materials", ec);
  const std::vector<unsigned char> png = white_png();
  return !ec && write_bytes(kTexturePath, png.data(), png.size()) &&
         write_text(std::string(kTexturePath) + ".meta", meta(kTextureGuid)) &&
         write_text(std::string(kMaterialPath) + ".meta",
                    meta(kMaterialGuid)) &&
         write_text(kMaterialPath,
                    std::string("{\"version\":4,\"roughness\":1.0,"
                                "\"textures\":{\"albedo\":\"") +
                        kTextureGuid + "\"}}");
}

void remove_project_files() {
  std::error_code ec{};
  std::filesystem::remove(kTexturePath, ec);
  std::filesystem::remove(std::string(kTexturePath) + ".meta", ec);
  std::filesystem::remove(kMaterialPath, ec);
  std::filesystem::remove(std::string(kMaterialPath) + ".meta", ec);
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

Entity add_cube(World &world, const engine::math::Vec3 &position) {
  engine::runtime::Transform transform{};
  transform.position = position;
  const Entity cube = world.create_scene_object(transform);
  engine::runtime::MeshComponent mesh{};
  mesh.meshAssetId = engine::content::make_asset_id_from_path("builtin://cube");
  mesh.materialAssetId =
      engine::content::make_asset_id_from_path(kMaterialPath);
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

  // The camera looks down -Z from the origin. The near cube is 4 units
  // away and to the left, the far one 40 units away straight ahead, so
  // their front faces lie left of centre and at the centre.
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.direction = engine::math::Vec3(0.0F, 0.0F, -1.0F);
  sunLight.intensity = 2.0F;
  if ((add_cube(world, engine::math::Vec3(-1.2F, 0.0F, -4.0F)) ==
       kInvalidEntity) ||
      (add_cube(world, engine::math::Vec3(0.0F, 0.0F, -40.0F)) ==
       kInvalidEntity) ||
      (sun == kInvalidEntity) || !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 0.0F, 0.0F),
                                engine::math::Vec3(0.0F, 0.0F, -1.0F))) {
    return 10;
  }

  CapturedFrame frame{};
  if (!settle_frames(pipeline, 30) ||
      !capture_presented_frame(pipeline, "mip_chain.tga", &frame)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }
  const int cx = static_cast<int>(frame.width / 2U);
  const int cy = static_cast<int>(frame.height / 2U);
  // The near cube's face spans x from about 0.27 to 0.40 of the frame;
  // sample the middle of it. The far face is sampled well inside its edge.
  const double nearLevel =
      block_level(frame, static_cast<int>((frame.width * 1U) / 3U), cy, 6);
  const double farLevel = block_level(frame, cx, cy, 3);
  std::printf("front faces: near %.1f, far %.1f\n", nearLevel, farLevel);
  if (nearLevel < 32.0) {
    std::fprintf(stderr, "FAIL: the near face is not lit (%.1f)\n", nearLevel);
    return 11;
  }
  // Same material, same light, same facing: only the mip level sampled
  // differs, and a uniform image is uniform at every level, so the two
  // match to within rounding: two levels of the 8-bit readback.
  if (std::fabs(nearLevel - farLevel) > 2.0) {
    std::fprintf(stderr,
                 "FAIL: the minified face differs from the magnified one "
                 "(%.1f against %.1f)\n",
                 farLevel, nearLevel);
    return 12;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::tests::detail::enter_asset_directory() ||
      !write_project_files()) {
    std::fprintf(stderr, "FAIL: could not write the test material\n");
    remove_project_files();
    return 1;
  }
  const int result =
      engine::tests::run_gpu_scene_test("texture_mip_chain_gpu_test", &run);
  remove_project_files();
  return result;
}
