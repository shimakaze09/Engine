// Verifies glTF skeleton import behavior for the Engine asset packer.

#include "skeleton_import.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <cgltf.h>

namespace {

constexpr const char *kGltfPath = "skeleton_import_test.gltf";
constexpr const char *kBinPath = "skeleton_import_test.bin";

/// Returns whether two floats are close enough for import validation.
bool almost_equal(float a, float b) noexcept {
  return std::fabs(static_cast<double>(a) - static_cast<double>(b)) < 0.00001;
}

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

/// Writes a temporary text file for inline glTF JSON.
bool write_text_file(const char *path, const char *text) noexcept {
  return write_binary_file(path, text, std::strlen(text));
}

/// Parses a temporary glTF file and optionally loads external buffers.
bool parse_gltf_file(const char *path, bool loadBuffers,
                     cgltf_data **outData) noexcept {
  if (outData == nullptr) {
    return false;
  }

  *outData = nullptr;
  cgltf_options options{};
  const cgltf_result parseResult = cgltf_parse_file(&options, path, outData);
  if ((parseResult != cgltf_result_success) || (*outData == nullptr)) {
    return false;
  }

  if (loadBuffers) {
    const cgltf_result loadResult =
        cgltf_load_buffers(&options, *outData, path);
    if (loadResult != cgltf_result_success) {
      cgltf_free(*outData);
      *outData = nullptr;
      return false;
    }
  }

  return true;
}

/// Verifies that glTF joints, hierarchy, and inverse binds import correctly.
int test_skin_joints_and_inverse_bind_matrices() noexcept {
  remove_file(kGltfPath);
  remove_file(kBinPath);

  std::array<float, 32U> matrices{};
  matrices[0U] = 1.0F;
  matrices[5U] = 1.0F;
  matrices[10U] = 1.0F;
  matrices[15U] = 1.0F;
  matrices[16U + 0U] = 1.0F;
  matrices[16U + 5U] = 1.0F;
  matrices[16U + 10U] = 1.0F;
  matrices[16U + 12U] = 2.0F;
  matrices[16U + 13U] = 3.0F;
  matrices[16U + 14U] = 4.0F;
  matrices[16U + 15U] = 1.0F;

  if (!write_binary_file(kBinPath, matrices.data(),
                         matrices.size() * sizeof(float))) {
    return 11;
  }

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"uri\":\"skeleton_import_test.bin\","
      "\"byteLength\":128}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
      "\"byteLength\":128}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
      "\"count\":2,\"type\":\"MAT4\"}],"
      "\"nodes\":[{\"name\":\"Root\",\"children\":[1]},"
      "{\"name\":\"Child\"}],"
      "\"skins\":[{\"skeleton\":0,\"joints\":[0,1],"
      "\"inverseBindMatrices\":0}]"
      "}";

  if (!write_text_file(kGltfPath, gltf)) {
    remove_file(kBinPath);
    return 12;
  }

  cgltf_data *data = nullptr;
  if (!parse_gltf_file(kGltfPath, true, &data)) {
    remove_file(kGltfPath);
    remove_file(kBinPath);
    return 13;
  }

  engine::tools::Skeleton skeleton{};
  engine::tools::SkeletonImportResult result =
      engine::tools::SkeletonImportResult::Ok;
  const bool parsed =
      engine::tools::parse_gltf_skeleton(data, 0U, &skeleton, &result);
  cgltf_free(data);
  remove_file(kGltfPath);
  remove_file(kBinPath);

  if (!parsed || (result != engine::tools::SkeletonImportResult::Ok)) {
    return 14;
  }
  if ((skeleton.rootJoint != 0U) || (skeleton.joints.size() != 2U)) {
    return 15;
  }
  if ((skeleton.joints[0U].name != "Root") ||
      (skeleton.joints[0U].parent != engine::tools::kInvalidSkeletonJoint)) {
    return 16;
  }
  if ((skeleton.joints[1U].name != "Child") ||
      (skeleton.joints[1U].parent != 0U)) {
    return 17;
  }
  if (!almost_equal(skeleton.joints[0U].inverseBindMatrix[0U], 1.0F) ||
      !almost_equal(skeleton.joints[1U].inverseBindMatrix[12U], 2.0F) ||
      !almost_equal(skeleton.joints[1U].inverseBindMatrix[13U], 3.0F) ||
      !almost_equal(skeleton.joints[1U].inverseBindMatrix[14U], 4.0F)) {
    return 18;
  }

  return 0;
}

/// Verifies that missing inverse bind matrices default to identity transforms.
int test_skin_without_inverse_bind_uses_identity() noexcept {
  remove_file(kGltfPath);

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"nodes\":[{\"children\":[1]},{}],"
      "\"skins\":[{\"joints\":[0,1]}]"
      "}";

  if (!write_text_file(kGltfPath, gltf)) {
    return 21;
  }

  cgltf_data *data = nullptr;
  if (!parse_gltf_file(kGltfPath, false, &data)) {
    remove_file(kGltfPath);
    return 22;
  }

  engine::tools::Skeleton skeleton{};
  engine::tools::SkeletonImportResult result =
      engine::tools::SkeletonImportResult::Ok;
  const bool parsed =
      engine::tools::parse_gltf_skeleton(data, 0U, &skeleton, &result);
  cgltf_free(data);
  remove_file(kGltfPath);

  if (!parsed || (result != engine::tools::SkeletonImportResult::Ok)) {
    return 23;
  }
  if ((skeleton.joints.size() != 2U) || (skeleton.rootJoint != 0U)) {
    return 24;
  }
  if ((skeleton.joints[0U].name != "joint_0") ||
      (skeleton.joints[1U].name != "joint_1")) {
    return 25;
  }
  if (!almost_equal(skeleton.joints[1U].inverseBindMatrix[0U], 1.0F) ||
      !almost_equal(skeleton.joints[1U].inverseBindMatrix[5U], 1.0F) ||
      !almost_equal(skeleton.joints[1U].inverseBindMatrix[10U], 1.0F) ||
      !almost_equal(skeleton.joints[1U].inverseBindMatrix[15U], 1.0F)) {
    return 26;
  }

  return 0;
}

/// Verifies malformed inverse bind accessors fail explicitly.
int test_invalid_inverse_bind_accessor_fails() noexcept {
  remove_file(kGltfPath);

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"byteLength\":12}],"
      "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
      "\"byteLength\":12}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
      "\"count\":1,\"type\":\"VEC3\"}],"
      "\"nodes\":[{}],"
      "\"skins\":[{\"joints\":[0],\"inverseBindMatrices\":0}]"
      "}";

  if (!write_text_file(kGltfPath, gltf)) {
    return 31;
  }

  cgltf_data *data = nullptr;
  if (!parse_gltf_file(kGltfPath, false, &data)) {
    remove_file(kGltfPath);
    return 32;
  }

  engine::tools::Skeleton skeleton{};
  engine::tools::SkeletonImportResult result =
      engine::tools::SkeletonImportResult::Ok;
  const bool parsed =
      engine::tools::parse_gltf_skeleton(data, 0U, &skeleton, &result);
  cgltf_free(data);
  remove_file(kGltfPath);

  if (parsed) {
    return 33;
  }
  return (result ==
          engine::tools::SkeletonImportResult::InvalidInverseBindAccessor)
             ? 0
             : 34;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = test_skin_joints_and_inverse_bind_matrices();
  if (result != 0) {
    std::fprintf(stderr, "FAIL skin import with inverse binds: %d\n", result);
    return result;
  }

  result = test_skin_without_inverse_bind_uses_identity();
  if (result != 0) {
    std::fprintf(stderr, "FAIL skin import default inverse binds: %d\n",
                 result);
    return result;
  }

  result = test_invalid_inverse_bind_accessor_fails();
  if (result != 0) {
    std::fprintf(stderr, "FAIL skin import validation: %d\n", result);
    return result;
  }

  std::printf("All skeleton import tests passed\n");
  return 0;
}
