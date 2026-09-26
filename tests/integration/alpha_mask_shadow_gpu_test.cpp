// Verifies on a real render device that an alpha-mask material casts a
// cut-out shadow: the shadow depth pass used to write the whole triangle
// whatever the material's alphaMode, so a mask-mode card (foliage, a fence,
// a grate) cast a solid rectangle while its lit surface was cut out.
//
// A 2 m square quad with UVs hangs 3 m above a white floor under a sun
// shining almost straight down. Its mask-mode material's opacity mask is a
// 4x4 checkerboard, so its shadow is a checkerboard of 0.5 m cells. The
// camera looks at the floor from the side, below the quad, and the test
// walks the frame row through the shadow: a full-silhouette shadow is one
// dark band (two lit/dark transitions along the row), a cut-out one
// alternates dark and lit cells (at least four). The quad mesh, its
// material, the mask texture and their sidecars are written into the asset
// directory before bootstrap, where the mount walk catalogues a project's
// own. The quad is written as a cooked mesh with no cook stamp, so the walk
// reports it without an identity; the scene reaches it by path.

#include "../gpu_scene_fixture.h"

#include "engine/content/asset_identity.h"
#include "engine/core/mesh_asset.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace {

using engine::runtime::Entity;
using engine::runtime::kInvalidEntity;
using engine::runtime::World;
using engine::tests::CapturedFrame;

constexpr const char *kMeshPath = "assets/meshes/alpha_mask_shadow_quad.mesh";
constexpr const char *kTexturePath =
    "assets/textures/alpha_mask_shadow_checker.png";
constexpr const char *kMaterialPath =
    "assets/materials/alpha_mask_shadow_test.mat";
constexpr const char *kTextureGuid = "2c7e1f40-6a93-4d85-b0e2-5f18c9d3a674";
constexpr const char *kMaterialGuid = "3d8f2051-7ba4-4e96-81f3-6029dae4b785";

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

/// A cooked (v2: position, normal, UV) 2x2 quad in the XZ plane, facing
/// +Y, with UVs spanning [0,1] across it.
bool write_quad_mesh() {
  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion2;
  header.vertexCount = 4U;
  header.indexCount = 6U;
  // clang-format off
  const float vertices[4 * 8] = {
      -1.0F, 0.0F, -1.0F,  0.0F, 1.0F, 0.0F,  0.0F, 0.0F,
      -1.0F, 0.0F,  1.0F,  0.0F, 1.0F, 0.0F,  0.0F, 1.0F,
       1.0F, 0.0F,  1.0F,  0.0F, 1.0F, 0.0F,  1.0F, 1.0F,
       1.0F, 0.0F, -1.0F,  0.0F, 1.0F, 0.0F,  1.0F, 0.0F,
  };
  // clang-format on
  const std::uint32_t indices[6] = {0U, 1U, 2U, 0U, 2U, 3U};
  std::vector<unsigned char> bytes(sizeof(header) + sizeof(vertices) +
                                   sizeof(indices));
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + sizeof(header), vertices, sizeof(vertices));
  std::memcpy(bytes.data() + sizeof(header) + sizeof(vertices), indices,
              sizeof(indices));
  return write_bytes(kMeshPath, bytes.data(), bytes.size());
}

/// CRC-32 as PNG chunks use it.
std::uint32_t crc32(const std::vector<unsigned char> &bytes) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
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
  std::vector<unsigned char> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  png->insert(png->end(), body.begin(), body.end());
  put_u32(png, crc32(body));
}

/// A 4x4 RGB PNG whose red channel is a checkerboard of 0 and 255, one
/// texel per cell, stored uncompressed (deflate "stored" blocks).
std::vector<unsigned char> checker_png() {
  constexpr std::uint32_t kSize = 4U;
  std::vector<unsigned char> raw{};
  for (std::uint32_t y = 0U; y < kSize; ++y) {
    raw.push_back(0U); // filter: none
    for (std::uint32_t x = 0U; x < kSize; ++x) {
      const unsigned char value = (((x + y) % 2U) == 0U) ? 255U : 0U;
      raw.insert(raw.end(), {value, value, value});
    }
  }
  std::vector<unsigned char> zlib{0x78U, 0x01U, 1U};
  const std::size_t length = raw.size();
  zlib.push_back(static_cast<unsigned char>(length & 0xFFU));
  zlib.push_back(static_cast<unsigned char>((length >> 8U) & 0xFFU));
  zlib.push_back(static_cast<unsigned char>(~length & 0xFFU));
  zlib.push_back(static_cast<unsigned char>((~length >> 8U) & 0xFFU));
  zlib.insert(zlib.end(), raw.begin(), raw.end());
  std::uint32_t a = 1U;
  std::uint32_t b = 0U;
  for (const unsigned char byte : raw) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
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
  std::filesystem::create_directories("assets/meshes", ec);
  std::filesystem::create_directories("assets/textures", ec);
  std::filesystem::create_directories("assets/materials", ec);
  const std::vector<unsigned char> png = checker_png();
  return !ec && write_quad_mesh() &&
         write_bytes(kTexturePath, png.data(), png.size()) &&
         write_text(std::string(kTexturePath) + ".meta", meta(kTextureGuid)) &&
         write_text(std::string(kMaterialPath) + ".meta",
                    meta(kMaterialGuid)) &&
         write_text(kMaterialPath,
                    std::string("{\"version\":4,\"alphaMode\":\"mask\","
                                "\"alphaCutoff\":0.5,"
                                "\"textures\":{\"opacity\":\"") +
                        kTextureGuid + "\"}}");
}

void remove_project_files() {
  std::error_code ec{};
  std::filesystem::remove(kMeshPath, ec);
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

int run(engine::EnginePipeline &pipeline, World &world) noexcept {
  using engine::tests::capture_presented_frame;
  using engine::tests::checked;
  using engine::tests::settle_frames;

  checked(engine::core::cvar_set_string("r_fog_mode", "off"), "r_fog_mode");
  checked(engine::core::cvar_set_bool("r_bloom", false), "r_bloom");
  checked(engine::core::cvar_set_bool("r_ssao", false), "r_ssao");
  checked(engine::core::cvar_set_bool("r_fxaa", false), "r_fxaa");

  engine::runtime::Transform floorTransform{};
  engine::runtime::Transform quadTransform{};
  quadTransform.position = engine::math::Vec3(0.0F, 3.0F, 0.25F);
  const Entity floor =
      engine::tests::add_builtin_mesh(world, "builtin://plane", floorTransform,
                                      engine::math::Vec3(1.0F, 1.0F, 1.0F));
  const Entity quad = world.create_scene_object(quadTransform);
  engine::runtime::MeshComponent quadMesh{};
  quadMesh.meshAssetId = engine::content::make_asset_id_from_path(kMeshPath);
  quadMesh.materialAssetId =
      engine::content::make_asset_id_from_path(kMaterialPath);
  // Nearly straight down; the small tilt keeps the light's view basis
  // well defined.
  const Entity sun = world.create_scene_object();
  engine::runtime::LightComponent sunLight{};
  sunLight.direction = engine::math::Vec3(0.05F, -1.0F, 0.02F);
  sunLight.intensity = 3.0F;
  // The camera sits below the quad's height and looks at the floor under
  // it, so the quad itself stays above the view.
  if ((floor == kInvalidEntity) || (quad == kInvalidEntity) ||
      !world.add_mesh_component(quad, quadMesh) || (sun == kInvalidEntity) ||
      !world.add_light_component(sun, sunLight) ||
      !engine::tests::look_from(world, engine::math::Vec3(0.0F, 1.2F, 5.0F),
                                engine::math::Vec3(0.0F, 0.0F, 0.0F))) {
    return 10;
  }

  CapturedFrame frame{};
  if (!settle_frames(pipeline, 30) ||
      !capture_presented_frame(pipeline, "alpha_mask_shadow.tga", &frame)) {
    std::printf("SKIPPED: the device returned no back-buffer readback\n");
    return 0;
  }

  // The row through the floor point under the camera's aim, which crosses
  // the shadow's middle row of cells.
  const int cy = static_cast<int>(frame.height / 2U);
  const int width = static_cast<int>(frame.width);
  std::vector<double> levels{};
  double darkest = 1.0e9;
  double brightest = -1.0e9;
  for (int x = 4; x < width - 4; x += 2) {
    const double level = block_level(frame, x, cy, 2);
    levels.push_back(level);
    darkest = std::fmin(darkest, level);
    brightest = std::fmax(brightest, level);
  }
  std::printf("row: darkest %.1f, brightest %.1f\n", darkest, brightest);
  // The shadow has to be there at all, or the count below proves nothing:
  // the sun at intensity 3 on a white floor puts a lit cell far above a
  // shadowed one, and 48 levels is well under that gap.
  if ((brightest - darkest) < 48.0) {
    std::fprintf(stderr, "FAIL: no shadow on the floor row (%.1f to %.1f)\n",
                 darkest, brightest);
    return 11;
  }
  // Transitions between lit and dark, with a hysteresis of a quarter of
  // the range so filtering at a cell edge cannot count twice.
  const double low = darkest + (0.25 * (brightest - darkest));
  const double high = brightest - (0.25 * (brightest - darkest));
  int transitions = 0;
  bool dark = levels.front() < low;
  for (const double level : levels) {
    if (dark && (level > high)) {
      dark = false;
      ++transitions;
    } else if (!dark && (level < low)) {
      dark = true;
      ++transitions;
    }
  }
  std::printf("lit/dark transitions along the row: %d\n", transitions);
  if (transitions < 4) {
    std::fprintf(stderr,
                 "FAIL: the mask-mode quad cast a solid shadow (%d "
                 "transitions; a cut-out one has at least 4)\n",
                 transitions);
    return 12;
  }
  return 0;
}

} // namespace

int main() {
  if (!engine::tests::detail::enter_asset_directory() ||
      !write_project_files()) {
    std::fprintf(stderr, "FAIL: could not write the test assets\n");
    remove_project_files();
    return 1;
  }
  const int result =
      engine::tests::run_gpu_scene_test("alpha_mask_shadow_gpu_test", &run);
  remove_project_files();
  return result;
}
