// Verifies glTF animation clip import behavior for the Engine asset packer.

#include "animation_import.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <cgltf.h>

namespace {

constexpr const char *kGltfPath = "animation_import_test.gltf";
constexpr const char *kBinPath = "animation_import_test.bin";

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

/// Verifies translation, rotation, and scale channels import as clip tracks.
int test_transform_channels_import() noexcept {
  remove_file(kGltfPath);
  remove_file(kBinPath);

  std::array<float, 22U> payload{};
  payload[0U] = 0.0F;
  payload[1U] = 1.0F;

  payload[2U] = 0.0F;
  payload[3U] = 0.0F;
  payload[4U] = 0.0F;
  payload[5U] = 2.0F;
  payload[6U] = 3.0F;
  payload[7U] = 4.0F;

  payload[8U] = 0.0F;
  payload[9U] = 0.0F;
  payload[10U] = 0.0F;
  payload[11U] = 1.0F;
  payload[12U] = 0.0F;
  payload[13U] = 0.7071F;
  payload[14U] = 0.0F;
  payload[15U] = 0.7071F;

  payload[16U] = 1.0F;
  payload[17U] = 1.0F;
  payload[18U] = 1.0F;
  payload[19U] = 1.5F;
  payload[20U] = 1.5F;
  payload[21U] = 1.5F;

  if (!write_binary_file(kBinPath, payload.data(),
                         payload.size() * sizeof(float))) {
    return 11;
  }

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"uri\":\"animation_import_test.bin\","
      "\"byteLength\":88}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24},"
      "{\"buffer\":0,\"byteOffset\":32,\"byteLength\":32},"
      "{\"buffer\":0,\"byteOffset\":64,\"byteLength\":24}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
      "\"type\":\"SCALAR\"},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":2,"
      "\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":2,"
      "\"type\":\"VEC4\"},"
      "{\"bufferView\":3,\"componentType\":5126,\"count\":2,"
      "\"type\":\"VEC3\"}],"
      "\"nodes\":[{\"name\":\"Root\",\"children\":[1]},"
      "{\"name\":\"Child\"}],"
      "\"skins\":[{\"skeleton\":0,\"joints\":[0,1]}],"
      "\"animations\":[{\"name\":\"Walk\",\"samplers\":["
      "{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"},"
      "{\"input\":0,\"output\":2,\"interpolation\":\"STEP\"},"
      "{\"input\":0,\"output\":3}],\"channels\":["
      "{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}},"
      "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"rotation\"}},"
      "{\"sampler\":2,\"target\":{\"node\":0,\"path\":\"scale\"}}]}]"
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

  engine::tools::AnimClip clip{};
  engine::tools::AnimationImportResult result =
      engine::tools::AnimationImportResult::Ok;
  const bool parsed =
      engine::tools::parse_gltf_animation(data, 0U, 0U, &clip, &result);
  cgltf_free(data);
  remove_file(kGltfPath);
  remove_file(kBinPath);

  if (!parsed || (result != engine::tools::AnimationImportResult::Ok)) {
    return 14;
  }
  if ((clip.name != "Walk") || !almost_equal(clip.durationSeconds, 1.0F) ||
      (clip.tracks.size() != 3U)) {
    return 15;
  }

  const engine::tools::AnimTrack &translation = clip.tracks[0U];
  if ((translation.joint != 1U) ||
      (translation.target != engine::tools::AnimTrackTarget::Translation) ||
      (translation.interpolation != engine::tools::AnimInterpolation::Linear) ||
      (translation.times.size() != 2U) ||
      (translation.vec3Values.size() != 2U)) {
    return 16;
  }
  if (!almost_equal(translation.vec3Values[1U].x, 2.0F) ||
      !almost_equal(translation.vec3Values[1U].y, 3.0F) ||
      !almost_equal(translation.vec3Values[1U].z, 4.0F)) {
    return 17;
  }

  const engine::tools::AnimTrack &rotation = clip.tracks[1U];
  if ((rotation.joint != 1U) ||
      (rotation.target != engine::tools::AnimTrackTarget::Rotation) ||
      (rotation.interpolation != engine::tools::AnimInterpolation::Step) ||
      (rotation.quatValues.size() != 2U)) {
    return 18;
  }
  if (!almost_equal(rotation.quatValues[1U].y, 0.7071F) ||
      !almost_equal(rotation.quatValues[1U].w, 0.7071F)) {
    return 19;
  }

  const engine::tools::AnimTrack &scale = clip.tracks[2U];
  if ((scale.joint != 0U) ||
      (scale.target != engine::tools::AnimTrackTarget::Scale) ||
      (scale.vec3Values.size() != 2U) ||
      !almost_equal(scale.vec3Values[1U].x, 1.5F)) {
    return 20;
  }

  return 0;
}

/// Verifies cubic-spline tracks store in/out tangents and key values.
int test_cubic_spline_tangents_import() noexcept {
  remove_file(kGltfPath);
  remove_file(kBinPath);

  std::array<float, 20U> payload{};
  payload[0U] = 0.0F;
  payload[1U] = 2.0F;

  payload[2U] = -1.0F;
  payload[3U] = 0.0F;
  payload[4U] = 0.0F;
  payload[5U] = 0.0F;
  payload[6U] = 0.0F;
  payload[7U] = 0.0F;
  payload[8U] = 1.0F;
  payload[9U] = 0.0F;
  payload[10U] = 0.0F;
  payload[11U] = -1.0F;
  payload[12U] = 0.0F;
  payload[13U] = 0.0F;
  payload[14U] = 2.0F;
  payload[15U] = 0.0F;
  payload[16U] = 0.0F;
  payload[17U] = 1.0F;
  payload[18U] = 0.0F;
  payload[19U] = 0.0F;

  if (!write_binary_file(kBinPath, payload.data(),
                         payload.size() * sizeof(float))) {
    return 31;
  }

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"uri\":\"animation_import_test.bin\","
      "\"byteLength\":80}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
      "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":72}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
      "\"type\":\"SCALAR\"},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":6,"
      "\"type\":\"VEC3\"}],"
      "\"nodes\":[{\"name\":\"Root\"}],"
      "\"skins\":[{\"joints\":[0]}],"
      "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1,"
      "\"interpolation\":\"CUBICSPLINE\"}],\"channels\":["
      "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}]"
      "}";

  if (!write_text_file(kGltfPath, gltf)) {
    remove_file(kBinPath);
    return 32;
  }

  cgltf_data *data = nullptr;
  if (!parse_gltf_file(kGltfPath, true, &data)) {
    remove_file(kGltfPath);
    remove_file(kBinPath);
    return 33;
  }

  engine::tools::AnimClip clip{};
  const bool parsed =
      engine::tools::parse_gltf_animation(data, 0U, 0U, &clip, nullptr);
  cgltf_free(data);
  remove_file(kGltfPath);
  remove_file(kBinPath);

  if (!parsed || (clip.tracks.size() != 1U)) {
    return 34;
  }

  const engine::tools::AnimTrack &track = clip.tracks[0U];
  if ((track.interpolation != engine::tools::AnimInterpolation::CubicSpline) ||
      (track.vec3Values.size() != 2U) ||
      (track.inVec3Tangents.size() != 2U) ||
      (track.outVec3Tangents.size() != 2U)) {
    return 35;
  }

  if (!almost_equal(track.inVec3Tangents[0U].x, -1.0F) ||
      !almost_equal(track.vec3Values[1U].x, 2.0F) ||
      !almost_equal(track.outVec3Tangents[1U].x, 1.0F) ||
      !almost_equal(clip.durationSeconds, 2.0F)) {
    return 36;
  }

  return 0;
}

/// Verifies malformed animation channel accessors fail explicitly.
int test_invalid_output_accessor_fails() noexcept {
  remove_file(kGltfPath);

  const char *gltf =
      "{"
      "\"asset\":{\"version\":\"2.0\"},"
      "\"buffers\":[{\"byteLength\":24}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":4},"
      "{\"buffer\":0,\"byteOffset\":4,\"byteLength\":16}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":1,"
      "\"type\":\"SCALAR\"},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":1,"
      "\"type\":\"VEC4\"}],"
      "\"nodes\":[{}],"
      "\"skins\":[{\"joints\":[0]}],"
      "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
      "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
      "\"path\":\"translation\"}}]}]"
      "}";

  if (!write_text_file(kGltfPath, gltf)) {
    return 41;
  }

  cgltf_data *data = nullptr;
  if (!parse_gltf_file(kGltfPath, false, &data)) {
    remove_file(kGltfPath);
    return 42;
  }

  engine::tools::AnimClip clip{};
  engine::tools::AnimationImportResult result =
      engine::tools::AnimationImportResult::Ok;
  const bool parsed =
      engine::tools::parse_gltf_animation(data, 0U, 0U, &clip, &result);
  cgltf_free(data);
  remove_file(kGltfPath);

  if (parsed) {
    return 43;
  }
  return (result ==
          engine::tools::AnimationImportResult::InvalidOutputAccessor)
             ? 0
             : 44;
}

} // namespace

/// Runs this executable or test program.
int main() {
  int result = test_transform_channels_import();
  if (result != 0) {
    std::fprintf(stderr, "FAIL animation transform channels: %d\n", result);
    return result;
  }

  result = test_cubic_spline_tangents_import();
  if (result != 0) {
    std::fprintf(stderr, "FAIL animation cubic spline: %d\n", result);
    return result;
  }

  result = test_invalid_output_accessor_fails();
  if (result != 0) {
    std::fprintf(stderr, "FAIL animation validation: %d\n", result);
    return result;
  }

  std::printf("All animation import tests passed\n");
  return 0;
}
