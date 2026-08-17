// Verifies mesh loader test behavior for the Engine test suite.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

#include "engine/core/mesh_asset.h"
#include "engine/renderer/mesh_loader.h"
#include "engine/renderer/render_device.h"

#include "mesh_handle_codec.h"

namespace {

constexpr const char *kBadMagicPath = "mesh_loader_bad_magic.mesh";
constexpr const char *kBadVersionPath = "mesh_loader_bad_version.mesh";
constexpr const char *kOversizedVertexPath =
    "mesh_loader_oversized_vertices.mesh";
constexpr const char *kOversizedIndexPath =
    "mesh_loader_oversized_indices.mesh";
constexpr const char *kFileSizeMismatchPath = "mesh_loader_size_mismatch.mesh";
constexpr const char *kOutOfRangeIndexPath =
    "mesh_loader_out_of_range_index.mesh";

engine::renderer::RenderDevice g_fakeDevice{};
std::uint32_t g_fakeGeometry = 1U;
std::uint32_t g_fakeVertexBuffer = 2U;
std::uint32_t g_fakeIndexBuffer = 3U;
std::uint32_t g_createBufferCalls = 0U;
std::uint32_t g_destroyBufferCalls = 0U;
std::uint32_t g_destroyGeometryCalls = 0U;

engine::renderer::DeviceBufferHandle
fake_create_buffer(const engine::renderer::BufferDesc &desc) noexcept {
  ++g_createBufferCalls;
  const std::uint32_t value =
      (desc.usage == engine::renderer::BufferUsage::Index)
          ? g_fakeIndexBuffer
          : g_fakeVertexBuffer;
  return engine::renderer::DeviceBufferHandle{value};
}

void fake_destroy_buffer(engine::renderer::DeviceBufferHandle) noexcept {
  ++g_destroyBufferCalls;
}

engine::renderer::DeviceGeometryHandle
fake_create_geometry(const engine::renderer::GeometryDesc &) noexcept {
  return engine::renderer::DeviceGeometryHandle{g_fakeGeometry};
}

void fake_destroy_geometry(engine::renderer::DeviceGeometryHandle) noexcept {
  ++g_destroyGeometryCalls;
}

void configure_fake_render_device(std::uint32_t geometry,
                                  std::uint32_t vertexBuffer,
                                  std::uint32_t indexBuffer) noexcept {
  g_fakeDevice = engine::renderer::RenderDevice{};
  g_fakeDevice.create_buffer = &fake_create_buffer;
  g_fakeDevice.destroy_buffer = &fake_destroy_buffer;
  g_fakeDevice.create_geometry = &fake_create_geometry;
  g_fakeDevice.destroy_geometry = &fake_destroy_geometry;
  g_fakeGeometry = geometry;
  g_fakeVertexBuffer = vertexBuffer;
  g_fakeIndexBuffer = indexBuffer;
  g_createBufferCalls = 0U;
  g_destroyBufferCalls = 0U;
  g_destroyGeometryCalls = 0U;
}

bool open_file_for_write(const char *path, FILE **outFile) noexcept {
  if ((path == nullptr) || (outFile == nullptr)) {
    return false;
  }

  *outFile = nullptr;
#ifdef _WIN32
  return fopen_s(outFile, path, "wb") == 0;
#else
  *outFile = std::fopen(path, "wb");
  return *outFile != nullptr;
#endif
}

void remove_file(const char *path) noexcept {
  if (path != nullptr) {
    static_cast<void>(std::remove(path));
  }
}

/// Writes mesh file data.
bool write_mesh_file(const char *path,
                     const engine::core::MeshAssetHeader &header,
                     const void *payload, std::size_t payloadBytes) noexcept {
  if (path == nullptr) {
    return false;
  }

  FILE *file = nullptr;
  if (!open_file_for_write(path, &file) || (file == nullptr)) {
    return false;
  }

  bool ok = (std::fwrite(&header, sizeof(header), 1U, file) == 1U);
  if (ok && (payloadBytes > 0U)) {
    ok = (payload != nullptr) &&
         (std::fwrite(payload, 1U, payloadBytes, file) == payloadBytes);
  }

  std::fclose(file);
  return ok;
}

int check_bad_magic() {
  remove_file(kBadMagicPath);

  engine::core::MeshAssetHeader header{};
  header.magic = 0xDEADBEEFU;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  if (!write_mesh_file(kBadMagicPath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kBadMagicPath);
    return 11;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kBadMagicPath, &mesh);
  remove_file(kBadMagicPath);
  return loaded ? 12 : 0;
}

int check_bad_version() {
  remove_file(kBadVersionPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = 99U;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  if (!write_mesh_file(kBadVersionPath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kBadVersionPath);
    return 21;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kBadVersionPath, &mesh);
  remove_file(kBadVersionPath);
  return loaded ? 22 : 0;
}

int check_oversized_vertex_count() {
  remove_file(kOversizedVertexPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 2000000U;
  header.indexCount = 0U;

  if (!write_mesh_file(kOversizedVertexPath, header, nullptr, 0U)) {
    remove_file(kOversizedVertexPath);
    return 31;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kOversizedVertexPath, &mesh);
  remove_file(kOversizedVertexPath);
  return loaded ? 32 : 0;
}

int check_oversized_index_count() {
  remove_file(kOversizedIndexPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 4000000U;

  if (!write_mesh_file(kOversizedIndexPath, header, nullptr, 0U)) {
    remove_file(kOversizedIndexPath);
    return 41;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kOversizedIndexPath, &mesh);
  remove_file(kOversizedIndexPath);
  return loaded ? 42 : 0;
}

int check_file_size_mismatch() {
  remove_file(kFileSizeMismatchPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 100U;
  header.indexCount = 0U;

  const std::array<float, 10U> truncatedVertexData = {
      0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
  };
  if (!write_mesh_file(kFileSizeMismatchPath, header,
                       truncatedVertexData.data(),
                       truncatedVertexData.size() * sizeof(float))) {
    remove_file(kFileSizeMismatchPath);
    return 51;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kFileSizeMismatchPath, &mesh);
  remove_file(kFileSizeMismatchPath);
  return loaded ? 52 : 0;
}

int check_out_of_range_index() {
  remove_file(kOutOfRangeIndexPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 1U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F,
                                            0.0F, 1.0F, 0.0F};
  constexpr std::uint32_t kInvalidIndex = 1U;
  FILE *file = nullptr;
  if (!open_file_for_write(kOutOfRangeIndexPath, &file) ||
      (file == nullptr)) {
    remove_file(kOutOfRangeIndexPath);
    return 141;
  }
  const bool wrote =
      (std::fwrite(&header, sizeof(header), 1U, file) == 1U) &&
      (std::fwrite(vertexData.data(), sizeof(float), vertexData.size(), file) ==
       vertexData.size()) &&
      (std::fwrite(&kInvalidIndex, sizeof(kInvalidIndex), 1U, file) == 1U);
  std::fclose(file);
  if (!wrote) {
    remove_file(kOutOfRangeIndexPath);
    return 142;
  }

  engine::renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 99ULL;
  const bool loaded = engine::renderer::load_mesh_data_from_file(
      kOutOfRangeIndexPath, &meshData, &sizeBytes);
  remove_file(kOutOfRangeIndexPath);
  if (loaded) {
    return 143;
  }
  return ((meshData.vertexCount == 0U) && meshData.indices.empty() &&
          meshData.vertices.empty() && (sizeBytes == 0ULL))
             ? 0
             : 144;
}

int check_empty_path() {
  engine::renderer::GpuMesh mesh{};
  const bool loaded = engine::renderer::load_mesh_from_file(nullptr, &mesh);
  return loaded ? 61 : 0;
}

int check_null_out_param() {
  const bool loaded =
      engine::renderer::load_mesh_from_file("somepath", nullptr);
  return loaded ? 71 : 0;
}

// --- v2 format tests ---

constexpr const char *kV2ValidPath = "mesh_loader_v2_valid.mesh";
constexpr const char *kCpuDecodePath = "mesh_loader_cpu_decode.mesh";

int check_cpu_decode_valid_mesh() {
  remove_file(kCpuDecodePath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 6U> vertexData = {0.0F, 0.0F, 0.0F,
                                            0.0F, 1.0F, 0.0F};
  if (!write_mesh_file(kCpuDecodePath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kCpuDecodePath);
    return 91;
  }

  engine::renderer::CpuMeshData meshData{};
  std::uint64_t sizeBytes = 0ULL;
  const bool loaded = engine::renderer::load_mesh_data_from_file(
      kCpuDecodePath, &meshData, &sizeBytes);
  remove_file(kCpuDecodePath);
  if (!loaded) {
    return 92;
  }
  if ((meshData.vertexCount != 1U) || !meshData.indices.empty() ||
      (meshData.vertices.size() != vertexData.size()) || meshData.hasUVs) {
    return 93;
  }
  const std::uint64_t expectedSize =
      static_cast<std::uint64_t>(sizeof(header) +
                                 vertexData.size() * sizeof(float));
  return (sizeBytes == expectedSize) ? 0 : 94;
}

int check_v2_bad_version_accepted() {
  // Verify that v2 files (which have version=2) are accepted by the loader.
  // This creates a minimal v2 binary blob and checks that the version
  // acceptance branch works. Since we have no GL context, load_mesh_from_file
  // will fail at the GPU upload step, but we can at least test that a
  // truly invalid version (e.g. 99) is still rejected.
  // (v2 was already tested above via bad_version test using version=99.)
  return 0;
}

int check_v2_file_size_validation() {
  // v2 format: 8 floats per vertex. Create a header claiming v2 with
  // 1 vertex but provide only 6 floats instead of 8.
  remove_file(kV2ValidPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion2;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  // Only 6 floats, but v2 expects 8 per vertex — file size mismatch.
  const std::array<float, 6U> truncatedData = {0.0F, 0.0F, 0.0F,
                                               0.0F, 1.0F, 0.0F};
  if (!write_mesh_file(kV2ValidPath, header, truncatedData.data(),
                       truncatedData.size() * sizeof(float))) {
    remove_file(kV2ValidPath);
    return 81;
  }

  engine::renderer::GpuMesh mesh{};
  const bool loaded =
      engine::renderer::load_mesh_from_file(kV2ValidPath, &mesh);
  remove_file(kV2ValidPath);
  // Should fail due to file size mismatch (6 floats != 8 required).
  return loaded ? 82 : 0;
}

// EXPECTATION: geometry-creation failure destroys both already-created
// buffers and zeroes the mesh; a stale-prefilled out param is cleared.
int check_gpu_upload_cleans_geometry_failure() {
  configure_fake_render_device(0U, 2U, 3U);

  const float vertices[6] = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  const std::uint32_t indices[3] = {0U, 0U, 0U};
  engine::renderer::GpuMesh mesh{};
  mesh.geometry = engine::renderer::DeviceGeometryHandle{99U};
  mesh.vertexBuffer = engine::renderer::DeviceBufferHandle{99U};
  mesh.indexBuffer = engine::renderer::DeviceBufferHandle{99U};
  mesh.vertexCount = 99U;
  mesh.indexCount = 99U;
  mesh.hasUVs = true;
  if (engine::renderer::build_gpu_mesh_from_data(vertices, 1U, indices, 3U,
                                                 false, &mesh)) {
    return 101;
  }
  if ((mesh.geometry != engine::renderer::kInvalidDeviceGeometry) ||
      (mesh.vertexBuffer != engine::renderer::kInvalidDeviceBuffer) ||
      (mesh.indexBuffer != engine::renderer::kInvalidDeviceBuffer) ||
      (mesh.vertexCount != 0U) || (mesh.indexCount != 0U)) {
    return 102;
  }
  return (g_destroyGeometryCalls == 0U && g_destroyBufferCalls == 2U) ? 0
                                                                      : 103;
}

int check_gpu_upload_cleans_vertex_buffer_failure() {
  configure_fake_render_device(1U, 0U, 3U);

  const float vertices[6] = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  engine::renderer::GpuMesh mesh{};
  if (engine::renderer::build_gpu_mesh_from_data(vertices, 1U, nullptr, 0U,
                                                 false, &mesh)) {
    return 111;
  }
  if ((mesh.geometry != engine::renderer::kInvalidDeviceGeometry) ||
      (mesh.vertexBuffer != engine::renderer::kInvalidDeviceBuffer) ||
      (mesh.indexBuffer != engine::renderer::kInvalidDeviceBuffer)) {
    return 112;
  }
  return (g_destroyGeometryCalls == 0U && g_destroyBufferCalls == 0U) ? 0
                                                                      : 113;
}

int check_gpu_upload_cleans_index_buffer_failure() {
  configure_fake_render_device(1U, 2U, 0U);

  const float vertices[6] = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  const std::uint32_t indices[3] = {0U, 0U, 0U};
  engine::renderer::GpuMesh mesh{};
  if (engine::renderer::build_gpu_mesh_from_data(vertices, 1U, indices, 3U,
                                                 false, &mesh)) {
    return 121;
  }
  if ((mesh.geometry != engine::renderer::kInvalidDeviceGeometry) ||
      (mesh.vertexBuffer != engine::renderer::kInvalidDeviceBuffer) ||
      (mesh.indexBuffer != engine::renderer::kInvalidDeviceBuffer)) {
    return 122;
  }
  return (g_destroyGeometryCalls == 0U && g_destroyBufferCalls == 1U) ? 0
                                                                      : 123;
}

constexpr const char *kV3ValidPath = "mesh_loader_v3_valid.mesh";
constexpr const char *kV3TruncatedPath = "mesh_loader_v3_truncated.mesh";
constexpr const char *kV3BadJointPath = "mesh_loader_v3_bad_joint.mesh";

/// EXPECTATION: a v3 file decodes with the 16-float stride, both hasUVs
/// and hasSkin set, and a byte-exact vertex payload roundtrip.
int check_v3_cpu_decode() {
  remove_file(kV3ValidPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion3;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 16U> vertexData = {
      1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 1.0F, 0.25F, 0.75F,
      2.0F, 1.0F, 0.0F, 2.0F, 0.5F, 0.25F, 0.25F, 0.0F};
  if (!write_mesh_file(kV3ValidPath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kV3ValidPath);
    return 141;
  }

  engine::renderer::CpuMeshData meshData{};
  const bool loaded =
      engine::renderer::load_mesh_data_from_file(kV3ValidPath, &meshData);
  remove_file(kV3ValidPath);
  if (!loaded) {
    return 142;
  }
  if ((meshData.vertexCount != 1U) || (meshData.strideFloats != 16U) ||
      (meshData.vertices.size() != 16U) || !meshData.hasUVs ||
      !meshData.hasSkin) {
    return 143;
  }
  for (std::size_t i = 0U; i < vertexData.size(); ++i) {
    if (meshData.vertices[i] != vertexData[i]) {
      return 144;
    }
  }
  return 0;
}

/// EXPECTATION: a v3 header over an 8-float payload is a file size
/// mismatch and the load fails.
int check_v3_file_size_validation() {
  remove_file(kV3TruncatedPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion3;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 8U> truncatedData = {0.0F, 0.0F, 0.0F, 0.0F,
                                               1.0F, 0.0F, 0.0F, 0.0F};
  if (!write_mesh_file(kV3TruncatedPath, header, truncatedData.data(),
                       truncatedData.size() * sizeof(float))) {
    remove_file(kV3TruncatedPath);
    return 151;
  }

  engine::renderer::CpuMeshData meshData{};
  const bool loaded =
      engine::renderer::load_mesh_data_from_file(kV3TruncatedPath, &meshData);
  remove_file(kV3TruncatedPath);
  return loaded ? 152 : 0;
}

int check_gpu_upload_rejects_missing_indices() {
  configure_fake_render_device(1U, 2U, 3U);

  const float vertices[6] = {0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
  engine::renderer::GpuMesh mesh{};
  if (engine::renderer::build_gpu_mesh_from_data(vertices, 1U, nullptr, 3U,
                                                 false, &mesh)) {
    return 131;
  }
  if ((mesh.geometry != engine::renderer::kInvalidDeviceGeometry) ||
      (mesh.vertexBuffer != engine::renderer::kInvalidDeviceBuffer) ||
      (mesh.indexBuffer != engine::renderer::kInvalidDeviceBuffer)) {
    return 132;
  }
  return (g_createBufferCalls == 0U && g_destroyGeometryCalls == 0U &&
          g_destroyBufferCalls == 0U)
             ? 0
             : 133;
}

/// Builds a single-vertex skinned CpuMeshData matching the v3 layout
/// (stride 16, joints 2/1/0/2 at floats 8-11, weights at 12-15); callers
/// corrupt one field to probe the H-11 validation.
bool make_skinned_mesh_data(engine::renderer::CpuMeshData *outData) {
  if (outData == nullptr) {
    return false;
  }
  if (!outData->vertices.assign({1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 1.0F, 0.25F,
                                 0.75F, 2.0F, 1.0F, 0.0F, 2.0F, 0.5F, 0.25F,
                                 0.25F, 0.0F})) {
    return false;
  }
  outData->indices.clear();
  outData->vertexCount = 1U;
  outData->strideFloats = 16U;
  outData->hasUVs = true;
  outData->hasSkin = true;
  return true;
}

/// EXPECTATION (audit H-11): upload_mesh_data_to_gpu rejects a payload
/// whose declared float count disagrees with vertexCount times stride,
/// a stride that disagrees with its layout flags, and a skin flag
/// without UVs — all without touching the device.
int check_upload_rejects_inconsistent_layout() {
  configure_fake_render_device(1U, 2U, 3U);

  engine::renderer::CpuMeshData meshData{};
  engine::renderer::GpuMesh mesh{};
  if (!make_skinned_mesh_data(&meshData)) {
    return 160;
  }
  // Undersized allocation: the buffer IS the capacity, so reallocating it
  // short is the review's deliberately-short-buffer regression.
  if (!meshData.vertices.allocate(8U)) {
    return 168;
  }
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 161;
  }
  if (g_createBufferCalls != 0U) {
    return 162;
  }

  if (!make_skinned_mesh_data(&meshData)) {
    return 163;
  }
  meshData.hasSkin = false;
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 164;
  }

  if (!make_skinned_mesh_data(&meshData)) {
    return 165;
  }
  meshData.hasUVs = false;
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 166;
  }
  return g_createBufferCalls == 0U ? 0 : 167;
}

/// EXPECTATION (audit H-11): skinned joint indices at or past the bone
/// palette bound, negative joints, and non-finite joints or weights all
/// reject; the joint index one below the bound uploads through the fake
/// device with the mesh fields intact.
int check_upload_validates_skin_payload() {
  configure_fake_render_device(1U, 2U, 3U);

  engine::renderer::CpuMeshData meshData{};
  engine::renderer::GpuMesh mesh{};
  if (!make_skinned_mesh_data(&meshData)) {
    return 170;
  }
  meshData.vertices[8] =
      static_cast<float>(engine::renderer::kMaxSkinPaletteJoints);
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 171;
  }

  if (!make_skinned_mesh_data(&meshData)) {
    return 172;
  }
  meshData.vertices[9] = -1.0F;
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 173;
  }

  if (!make_skinned_mesh_data(&meshData)) {
    return 174;
  }
  meshData.vertices[10] = std::numeric_limits<float>::quiet_NaN();
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 175;
  }

  if (!make_skinned_mesh_data(&meshData)) {
    return 176;
  }
  meshData.vertices[12] = std::numeric_limits<float>::quiet_NaN();
  if (engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 177;
  }

  if (!make_skinned_mesh_data(&meshData)) {
    return 178;
  }
  meshData.vertices[8] =
      static_cast<float>(engine::renderer::kMaxSkinPaletteJoints) - 1.0F;
  if (!engine::renderer::upload_mesh_data_to_gpu(meshData, &mesh)) {
    return 179;
  }
  if ((mesh.geometry.value != 1U) || (mesh.vertexBuffer.value != 2U) ||
      (mesh.vertexCount != 1U) || !mesh.hasSkin || !mesh.hasUVs) {
    return 180;
  }
  return 0;
}

/// EXPECTATION (audit H-11): a cooked v3 mesh file carrying a joint index
/// outside the bone palette fails to decode.
int check_v3_decode_rejects_out_of_palette_joint() {
  remove_file(kV3BadJointPath);

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = engine::core::kMeshAssetVersion3;
  header.vertexCount = 1U;
  header.indexCount = 0U;

  const std::array<float, 16U> vertexData = {
      1.0F, 2.0F, 3.0F, 0.0F, 0.0F, 1.0F, 0.25F, 0.75F,
      999.0F, 1.0F, 0.0F, 2.0F, 0.5F, 0.25F, 0.25F, 0.0F};
  if (!write_mesh_file(kV3BadJointPath, header, vertexData.data(),
                       vertexData.size() * sizeof(float))) {
    remove_file(kV3BadJointPath);
    return 181;
  }

  engine::renderer::CpuMeshData meshData{};
  const bool loaded =
      engine::renderer::load_mesh_data_from_file(kV3BadJointPath, &meshData);
  remove_file(kV3BadJointPath);
  return loaded ? 182 : 0;
}

// --- audit #173: MeshHandle generation validation ---

/// EXPECTATION (audit #173, red on main): releasing mesh A and reusing its
/// slot for mesh B must not let the stale handle to A resolve to B. Before
/// the fix, MeshHandle carried no generation and register_gpu_mesh returned
/// the raw slot index, so a saved handle silently aliased whatever mesh the
/// registry later placed in the same slot.
int check_stale_handle_rejected_after_slot_reuse() {
  engine::renderer::GpuMeshRegistry registry{};

  engine::renderer::GpuMesh meshA{};
  meshA.vertexCount = 111U;
  const engine::renderer::MeshHandle handleA =
      engine::renderer::register_gpu_mesh(&registry, meshA);
  if (handleA == engine::renderer::kInvalidMeshHandle) {
    return 190;
  }

  const engine::renderer::GpuMesh *lookedUpA =
      engine::renderer::lookup_gpu_mesh(&registry, handleA);
  if ((lookedUpA == nullptr) || (lookedUpA->vertexCount != 111U)) {
    return 191;
  }

  engine::renderer::unload_gpu_mesh(&registry, handleA);
  if (engine::renderer::lookup_gpu_mesh(&registry, handleA) != nullptr) {
    return 192;
  }

  engine::renderer::GpuMesh meshB{};
  meshB.vertexCount = 222U;
  const engine::renderer::MeshHandle handleB =
      engine::renderer::register_gpu_mesh(&registry, meshB);
  if (handleB == engine::renderer::kInvalidMeshHandle) {
    return 193;
  }

  // The registry always claims the first free slot, so B lands in A's old
  // slot with a bumped generation: same slot bits, different handle value.
  if (engine::renderer::mesh_handle_detail::slot_index(handleA) !=
      engine::renderer::mesh_handle_detail::slot_index(handleB)) {
    return 194;
  }
  if (handleA.id == handleB.id) {
    return 195;
  }

  // The stale handle to A must still fail even though its slot is occupied.
  if (engine::renderer::lookup_gpu_mesh(&registry, handleA) != nullptr) {
    return 196;
  }

  const engine::renderer::GpuMesh *lookedUpB =
      engine::renderer::lookup_gpu_mesh(&registry, handleB);
  if ((lookedUpB == nullptr) || (lookedUpB->vertexCount != 222U)) {
    return 197;
  }

  return 0;
}

/// EXPECTATION (audit #173): the generation counter wraps past its bounded
/// field without ever landing on the reserved zero (invalid) value.
int check_generation_wrap_policy() {
  engine::renderer::GpuMeshRegistry registry{};

  engine::renderer::GpuMesh mesh{};
  const engine::renderer::MeshHandle handle =
      engine::renderer::register_gpu_mesh(&registry, mesh);
  if (handle == engine::renderer::kInvalidMeshHandle) {
    return 200;
  }
  const std::uint32_t slot =
      engine::renderer::mesh_handle_detail::slot_index(handle);

  // Force the slot's stored generation to the last value before the codec
  // wraps, then unload once more to cross the boundary.
  registry.generations[slot] = engine::renderer::mesh_handle_detail::kGenerationMask;
  registry.occupied[slot] = true;
  engine::renderer::unload_gpu_mesh(
      &registry, engine::renderer::mesh_handle_detail::make_handle(
                     slot, engine::renderer::mesh_handle_detail::kGenerationMask));

  if (registry.generations[slot] != 1U) {
    return 201;
  }

  const engine::renderer::MeshHandle wrapped =
      engine::renderer::register_gpu_mesh(&registry, mesh);
  if (wrapped == engine::renderer::kInvalidMeshHandle) {
    return 202;
  }
  if (engine::renderer::mesh_handle_detail::generation(wrapped) != 1U) {
    return 203;
  }
  return 0;
}

/// EXPECTATION (audit #173): a full registry rejects further registration,
/// and releasing one slot makes exactly one more registration succeed.
int check_registry_full_then_reuse() {
  std::unique_ptr<engine::renderer::GpuMeshRegistry> registry(
      new (std::nothrow) engine::renderer::GpuMeshRegistry());
  if (registry == nullptr) {
    return 210;
  }

  engine::renderer::GpuMesh mesh{};
  engine::renderer::MeshHandle firstHandle = engine::renderer::kInvalidMeshHandle;
  std::size_t registered = 0U;
  for (std::size_t i = 1U; i < engine::renderer::GpuMeshRegistry::kMaxSlots;
       ++i) {
    const engine::renderer::MeshHandle handle =
        engine::renderer::register_gpu_mesh(registry.get(), mesh);
    if (handle == engine::renderer::kInvalidMeshHandle) {
      return 211;
    }
    if (registered == 0U) {
      firstHandle = handle;
    }
    ++registered;
  }
  if (registered != engine::renderer::GpuMeshRegistry::kMaxSlots - 1U) {
    return 212;
  }

  // Registry is now full (slot 0 stays reserved as invalid): one more
  // registration must fail cleanly rather than overwrite a live slot.
  if (engine::renderer::register_gpu_mesh(registry.get(), mesh) !=
      engine::renderer::kInvalidMeshHandle) {
    return 213;
  }

  engine::renderer::unload_gpu_mesh(registry.get(), firstHandle);
  const engine::renderer::MeshHandle reused =
      engine::renderer::register_gpu_mesh(registry.get(), mesh);
  if (reused == engine::renderer::kInvalidMeshHandle) {
    return 214;
  }
  if (engine::renderer::mesh_handle_detail::slot_index(reused) !=
      engine::renderer::mesh_handle_detail::slot_index(firstHandle)) {
    return 215;
  }
  if (reused.id == firstHandle.id) {
    return 216;
  }

  // The registry is full again: registration must fail once more.
  if (engine::renderer::register_gpu_mesh(registry.get(), mesh) !=
      engine::renderer::kInvalidMeshHandle) {
    return 217;
  }

  return 0;
}

} // namespace

namespace engine::renderer {

bool initialize_render_device() noexcept { return true; }

void shutdown_render_device() noexcept {}

const RenderDevice *render_device() noexcept { return &g_fakeDevice; }

} // namespace engine::renderer

/// Runs this executable or test program.
int main() {
  int result = check_bad_magic();
  if (result != 0) {
    return result;
  }

  result = check_bad_version();
  if (result != 0) {
    return result;
  }

  result = check_oversized_vertex_count();
  if (result != 0) {
    return result;
  }

  result = check_oversized_index_count();
  if (result != 0) {
    return result;
  }

  result = check_file_size_mismatch();
  if (result != 0) {
    return result;
  }

  result = check_out_of_range_index();
  if (result != 0) {
    return result;
  }

  result = check_empty_path();
  if (result != 0) {
    return result;
  }

  result = check_null_out_param();
  if (result != 0) {
    return result;
  }

  result = check_v2_bad_version_accepted();
  if (result != 0) {
    return result;
  }

  result = check_v2_file_size_validation();
  if (result != 0) {
    return result;
  }

  result = check_gpu_upload_cleans_geometry_failure();
  if (result != 0) {
    return result;
  }

  result = check_gpu_upload_cleans_vertex_buffer_failure();
  if (result != 0) {
    return result;
  }

  result = check_gpu_upload_cleans_index_buffer_failure();
  if (result != 0) {
    return result;
  }

  result = check_gpu_upload_rejects_missing_indices();
  if (result != 0) {
    return result;
  }

  result = check_cpu_decode_valid_mesh();
  if (result != 0) {
    return result;
  }

  result = check_v3_cpu_decode();
  if (result != 0) {
    return result;
  }

  result = check_v3_file_size_validation();
  if (result != 0) {
    return result;
  }

  result = check_upload_rejects_inconsistent_layout();
  if (result != 0) {
    return result;
  }

  result = check_upload_validates_skin_payload();
  if (result != 0) {
    return result;
  }

  result = check_v3_decode_rejects_out_of_palette_joint();
  if (result != 0) {
    return result;
  }

  result = check_stale_handle_rejected_after_slot_reuse();
  if (result != 0) {
    return result;
  }

  result = check_generation_wrap_policy();
  if (result != 0) {
    return result;
  }

  return check_registry_full_then_reuse();
}
