// Verifies skinned glTF mesh cooking: JOINTS_0/WEIGHTS_0 extraction into
// the 16-float v3 layout with joint indices remapped to the reordered
// skeleton and weights renormalized, the zero-filled uv slot when the
// source has no texcoords, opt-out to the unskinned layout when no remap
// is supplied, out-of-range joint rejection, and the v3 header on the
// written mesh file.

#include "packer_shared.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

#include <cgltf.h>

#include "engine/core/mesh_asset.h"

namespace {

constexpr const char *kGltfPath = "skinned_mesh_cook_test.gltf";
constexpr const char *kBinPath = "skinned_mesh_cook_test.bin";
constexpr const char *kMeshPath = "skinned_mesh_cook_test.mesh";

/// When set, the fixture's first position is NaN (#571 finiteness row).
bool g_poisonPosition = false;

/// Removes a temporary test file when it exists.
void remove_file(const char *path) noexcept {
  if (path != nullptr) {
    static_cast<void>(std::remove(path));
  }
}

/// Writes a temporary binary file for a glTF sidecar buffer.
bool write_binary_file(const char *path, const void *data,
                       std::size_t size) noexcept {
  FILE *file = nullptr;
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

  const bool ok = std::fwrite(data, 1U, size, file) == size;
  std::fclose(file);
  return ok;
}

/// Builds the sidecar buffer for one skinned triangle without texcoords:
/// positions, normals, ushort joint indices, and float weights.
bool write_skinned_fixture_bin() noexcept {
  std::vector<std::uint8_t> bin(144U, 0U);
  std::array<float, 9U> positions = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
                                     0.0F, 0.0F, 1.0F, 0.0F};
  if (g_poisonPosition) {
    positions[0U] = std::numeric_limits<float>::quiet_NaN();
  }
  const std::array<float, 9U> normals = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                         1.0F, 0.0F, 0.0F, 1.0F};
  const std::array<std::uint16_t, 12U> joints = {0U, 1U, 2U, 0U, 2U, 2U,
                                                 2U, 2U, 1U, 0U, 0U, 0U};
  const std::array<float, 12U> weights = {0.5F, 0.25F, 0.25F, 0.0F,
                                          2.0F, 2.0F,  0.0F,  0.0F,
                                          1.0F, 0.0F,  0.0F,  0.0F};
  std::memcpy(bin.data(), positions.data(), sizeof(positions));
  std::memcpy(bin.data() + 36U, normals.data(), sizeof(normals));
  std::memcpy(bin.data() + 72U, joints.data(), sizeof(joints));
  std::memcpy(bin.data() + 96U, weights.data(), sizeof(weights));
  return write_binary_file(kBinPath, bin.data(), bin.size());
}

/// Parses the fixture glTF and hands back its only primitive; the data
/// stays owned by outData and must be freed by the caller.
bool load_fixture_primitive(cgltf_data **outData,
                            const cgltf_primitive **outPrimitive) noexcept {
  if (!write_skinned_fixture_bin()) {
    return false;
  }

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"uri\":\"skinned_mesh_cook_test.bin\","
      "\"byteLength\":144}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
      "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
      "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":48}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
      "\"type\":\"VEC3\"},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":3,"
      "\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5123,\"count\":3,"
      "\"type\":\"VEC4\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":3,"
      "\"type\":\"VEC4\"}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{"
      "\"POSITION\":0,\"NORMAL\":1,\"JOINTS_0\":2,\"WEIGHTS_0\":3}}]}]"
      "}";
  if (!write_binary_file(kGltfPath, gltf, std::strlen(gltf))) {
    return false;
  }

  cgltf_options options{};
  *outData = nullptr;
  if ((cgltf_parse_file(&options, kGltfPath, outData) !=
       cgltf_result_success) ||
      (*outData == nullptr)) {
    return false;
  }
  if (cgltf_load_buffers(&options, *outData, kGltfPath) !=
      cgltf_result_success) {
    cgltf_free(*outData);
    *outData = nullptr;
    return false;
  }
  if (((*outData)->meshes_count != 1U) ||
      ((*outData)->meshes[0].primitives_count != 1U)) {
    cgltf_free(*outData);
    *outData = nullptr;
    return false;
  }
  *outPrimitive = &(*outData)->meshes[0].primitives[0];
  return true;
}

/// Deletes every fixture file this suite writes.
void cleanup_fixture_files() noexcept {
  remove_file(kGltfPath);
  remove_file(kBinPath);
  remove_file(kMeshPath);
}

/// EXPECTATION (#571): a non-finite vertex position is refused at
/// extraction, before it can reach the thumbnail rasterizer's int cast or
/// the hull builder.
int check_non_finite_position_rejected() {
  g_poisonPosition = true;
  cgltf_data *data = nullptr;
  const cgltf_primitive *primitive = nullptr;
  const bool loaded = load_fixture_primitive(&data, &primitive);
  g_poisonPosition = false;
  if (!loaded) {
    std::puts("poisoned fixture setup failed");
    return 1;
  }
  PrimitiveData cooked{};
  const bool extracted = extract_primitive(primitive, &cooked, nullptr);
  cgltf_free(data);
  if (extracted) {
    std::puts("a NaN position was accepted");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit H-20): upAxis conversion rotates positions and
/// normals with proper rotations — Z-up (0,0,1) lands exactly on Y-up
/// (0,1,0), X-up (1,0,0) likewise — and Y-up plus unknown values no-op.
/// Sign/swap maps are exact in floats, so assertions are exact.
int check_up_axis_applied() {
  PrimitiveData data{};
  data.interleavedVertices = {0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                              2.0F, 3.0F, 5.0F, 0.0F, 1.0F, 0.0F};

  apply_up_axis_to_primitive(&data, 2);
  if ((data.interleavedVertices[0] != 0.0F) ||
      (data.interleavedVertices[1] != 1.0F) ||
      (data.interleavedVertices[2] != 0.0F) ||
      (data.interleavedVertices[4] != 1.0F) ||
      (data.interleavedVertices[5] != 0.0F)) {
    std::puts("Z-up conversion wrong for first vertex");
    return 1;
  }
  if ((data.interleavedVertices[6] != 2.0F) ||
      (data.interleavedVertices[7] != 5.0F) ||
      (data.interleavedVertices[8] != -3.0F) ||
      (data.interleavedVertices[10] != 0.0F) ||
      (data.interleavedVertices[11] != -1.0F)) {
    std::puts("Z-up conversion wrong for second vertex");
    return 1;
  }

  PrimitiveData xUp{};
  xUp.interleavedVertices = {1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
  apply_up_axis_to_primitive(&xUp, 0);
  if ((xUp.interleavedVertices[0] != 0.0F) ||
      (xUp.interleavedVertices[1] != 1.0F) ||
      (xUp.interleavedVertices[2] != 0.0F) ||
      (xUp.interleavedVertices[4] != 1.0F)) {
    std::puts("X-up conversion wrong");
    return 1;
  }

  PrimitiveData untouched{};
  untouched.interleavedVertices = {1.0F, 2.0F, 3.0F, 0.0F, 1.0F, 0.0F};
  const std::vector<float> before = untouched.interleavedVertices;
  apply_up_axis_to_primitive(&untouched, 1);
  apply_up_axis_to_primitive(&untouched, 7);
  if (untouched.interleavedVertices != before) {
    std::puts("Y-up or unknown axis modified the primitive");
    return 1;
  }
  return 0;
}

/// EXPECTATION (audit H-20): generateNormals recomputes per-vertex
/// normals from triangle geometry — a CCW triangle in the XY plane gets
/// exactly (0,0,1) at every corner through both the indexed and the
/// sequential-triple paths, and a malformed index leaves normals zeroed
/// instead of reading out of bounds.
int check_generate_normals_applied() {
  PrimitiveData data{};
  data.hasUVs = true;
  data.interleavedVertices = {
      0.0F, 0.0F, 0.0F, 9.0F, 9.0F, 9.0F, 0.0F, 0.0F,
      1.0F, 0.0F, 0.0F, 9.0F, 9.0F, 9.0F, 0.0F, 0.0F,
      0.0F, 1.0F, 0.0F, 9.0F, 9.0F, 9.0F, 0.0F, 0.0F};
  data.indices = {0U, 1U, 2U};

  generate_normals_for_primitive(&data);
  for (std::size_t v = 0U; v < 3U; ++v) {
    const std::size_t base = v * 8U;
    if ((data.interleavedVertices[base + 3U] != 0.0F) ||
        (data.interleavedVertices[base + 4U] != 0.0F) ||
        (data.interleavedVertices[base + 5U] != 1.0F)) {
      std::puts("indexed normal generation wrong");
      return 1;
    }
  }

  data.indices.clear();
  generate_normals_for_primitive(&data);
  if ((data.interleavedVertices[5U] != 1.0F) ||
      (data.interleavedVertices[13U] != 1.0F)) {
    std::puts("unindexed normal generation wrong");
    return 1;
  }

  data.indices = {0U, 1U, 5U};
  generate_normals_for_primitive(&data);
  for (std::size_t v = 0U; v < 3U; ++v) {
    const std::size_t base = v * 8U;
    if ((data.interleavedVertices[base + 3U] != 0.0F) ||
        (data.interleavedVertices[base + 4U] != 0.0F) ||
        (data.interleavedVertices[base + 5U] != 0.0F)) {
      std::puts("malformed index did not leave normals zeroed");
      return 1;
    }
  }
  return 0;
}

/// EXPECTATION (audit H-20): the external .bin buffer payload the glTF
/// references lands in both the dependency graph and the auto digest
/// list, so editing vertex data without touching the .gltf still forces
/// a recook.
int check_external_buffer_becomes_dependency() {
  cgltf_data *data = nullptr;
  const cgltf_primitive *primitive = nullptr;
  if (!load_fixture_primitive(&data, &primitive)) {
    std::puts("fixture setup failed");
    return 1;
  }

  engine::tools::DependencyGraph graph{};
  std::vector<DependencyDigest> digests{};
  const std::uint64_t meshAssetId = hash_path_to_asset_id(kGltfPath);
  const bool extracted = extract_gltf_dependencies(data, kGltfPath,
                                                   meshAssetId, &graph,
                                                   &digests);
  cgltf_free(data);
  if (!extracted) {
    std::puts("dependency extraction failed");
    return 1;
  }

  bool binDigested = false;
  for (const DependencyDigest &digest : digests) {
    if ((digest.path.find(kBinPath) != std::string::npos) &&
        (digest.hash != 0ULL)) {
      binDigested = true;
      break;
    }
  }
  if (!binDigested) {
    std::puts("external buffer missing from dependency digests");
    return 1;
  }

  engine::tools::DependencyGraph::AssetId depIds[8] = {};
  const std::size_t depCount =
      engine::tools::get_dependencies(&graph, meshAssetId, depIds, 8U);
  bool binInGraph = false;
  for (std::size_t i = 0U; i < depCount; ++i) {
    auto pathIt = graph.assetPaths.find(depIds[i]);
    if ((pathIt != graph.assetPaths.end()) &&
        (pathIt->second.find(kBinPath) != std::string::npos)) {
      binInGraph = true;
      break;
    }
  }
  if (!binInGraph) {
    std::puts("external buffer missing from the dependency graph");
    return 1;
  }
  return 0;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = check_skinned_extraction();
  if (result == 0) {
    result = check_unskinned_without_remap();
  }
  if (result == 0) {
    result = check_out_of_range_joint_rejected();
  }
  if (result == 0) {
    result = check_v3_mesh_file_header();
  }
  if (result == 0) {
    result = check_non_finite_position_rejected();
  }
  if (result == 0) {
    result = check_external_buffer_becomes_dependency();
  }
  if (result == 0) {
    result = check_up_axis_applied();
  }
  if (result == 0) {
    result = check_generate_normals_applied();
  }
  if (result == 0) {
    result = check_non_triangle_mode_rejected();
  }
  if (result == 0) {
    result = check_missing_normals_with_generation();
  }
  if (result == 0) {
    result = check_hull_write_failure_reported();
  }
  remove_file("hull_skip_test.mesh.hull");
  cleanup_fixture_files();
  return result;
}
