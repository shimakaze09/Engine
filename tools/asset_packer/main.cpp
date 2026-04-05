#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "engine/core/mesh_asset.h"

namespace {

struct PrimitiveData final {
  std::vector<float> interleavedVertices{};
  std::vector<std::uint32_t> indices{};
  bool hasUVs = false;
};

void print_usage() {
  std::fprintf(stderr,
               "usage: asset_packer <input.gltf|input.glb> <output.mesh>\n");
}

const cgltf_accessor *find_attribute_accessor(const cgltf_primitive *primitive,
                                              cgltf_attribute_type type) {
  if (primitive == nullptr) {
    return nullptr;
  }

  for (cgltf_size i = 0; i < primitive->attributes_count; ++i) {
    const cgltf_attribute &attribute = primitive->attributes[i];
    if ((attribute.type == type) && (attribute.data != nullptr)) {
      return attribute.data;
    }
  }

  return nullptr;
}

bool extract_primitive(const cgltf_primitive *primitive,
                       PrimitiveData *outData) {
  if ((primitive == nullptr) || (outData == nullptr)) {
    return false;
  }

  const cgltf_accessor *positions =
      find_attribute_accessor(primitive, cgltf_attribute_type_position);
  const cgltf_accessor *normals =
      find_attribute_accessor(primitive, cgltf_attribute_type_normal);
  const cgltf_accessor *texcoords =
      find_attribute_accessor(primitive, cgltf_attribute_type_texcoord);
  if ((positions == nullptr) || (normals == nullptr)) {
    std::fprintf(stderr,
                 "error: primitive must have POSITION and NORMAL accessors\n");
    return false;
  }

  if ((positions->type != cgltf_type_vec3)
      || (normals->type != cgltf_type_vec3)) {
    std::fprintf(stderr, "error: POSITION and NORMAL must be vec3\n");
    return false;
  }

  if (positions->count != normals->count) {
    std::fprintf(stderr,
                 "error: POSITION and NORMAL vertex counts do not match\n");
    return false;
  }

  const bool hasUVs = (texcoords != nullptr)
                      && (texcoords->type == cgltf_type_vec2)
                      && (texcoords->count == positions->count);
  outData->hasUVs = hasUVs;
  const std::size_t strideFloats = hasUVs ? 8U : 6U;

  const std::size_t vertexCount = static_cast<std::size_t>(positions->count);
  outData->interleavedVertices.assign(vertexCount * strideFloats, 0.0F);

  std::array<float, 3U> position{};
  std::array<float, 3U> normal{};
  std::array<float, 2U> uv{};

  for (std::size_t i = 0U; i < vertexCount; ++i) {
    if (!cgltf_accessor_read_float(positions,
                                   static_cast<cgltf_size>(i),
                                   position.data(),
                                   position.size())
        || !cgltf_accessor_read_float(normals,
                                      static_cast<cgltf_size>(i),
                                      normal.data(),
                                      normal.size())) {
      std::fprintf(stderr, "error: failed to decode vertex attributes\n");
      return false;
    }

    const std::size_t base = i * strideFloats;
    outData->interleavedVertices[base + 0U] = position[0U];
    outData->interleavedVertices[base + 1U] = position[1U];
    outData->interleavedVertices[base + 2U] = position[2U];
    outData->interleavedVertices[base + 3U] = normal[0U];
    outData->interleavedVertices[base + 4U] = normal[1U];
    outData->interleavedVertices[base + 5U] = normal[2U];

    if (hasUVs) {
      if (!cgltf_accessor_read_float(
              texcoords, static_cast<cgltf_size>(i), uv.data(), uv.size())) {
        std::fprintf(stderr, "error: failed to decode UV attributes\n");
        return false;
      }
      outData->interleavedVertices[base + 6U] = uv[0U];
      outData->interleavedVertices[base + 7U] = uv[1U];
    }
  }

  if (primitive->indices != nullptr) {
    const std::size_t indexCount =
        static_cast<std::size_t>(primitive->indices->count);
    outData->indices.assign(indexCount, 0U);

    for (std::size_t i = 0U; i < indexCount; ++i) {
      const cgltf_uint index = cgltf_accessor_read_index(
          primitive->indices, static_cast<cgltf_size>(i));
      if (index >= positions->count) {
        std::fprintf(stderr, "error: index out of range\n");
        return false;
      }

      outData->indices[i] = static_cast<std::uint32_t>(index);
    }
  }

  return true;
}

bool write_mesh_file(const char *outputPath, const PrimitiveData &data) {
  if (outputPath == nullptr) {
    return false;
  }

  const std::size_t strideFloats = data.hasUVs ? 8U : 6U;
  if ((data.interleavedVertices.size() % strideFloats) != 0U) {
    std::fprintf(stderr, "error: interleaved vertex buffer is invalid\n");
    return false;
  }

  const std::size_t vertexCount =
      data.interleavedVertices.size() / strideFloats;
  if (vertexCount > static_cast<std::size_t>(UINT32_MAX)
      || (data.indices.size() > static_cast<std::size_t>(UINT32_MAX))) {
    std::fprintf(stderr, "error: mesh exceeds supported format limits\n");
    return false;
  }

  FILE *outputFile = nullptr;
#ifdef _WIN32
  if (fopen_s(&outputFile, outputPath, "wb") != 0) {
    outputFile = nullptr;
  }
#else
  outputFile = std::fopen(outputPath, "wb");
#endif
  if (outputFile == nullptr) {
    std::fprintf(stderr, "error: failed to open output file: %s\n", outputPath);
    return false;
  }

  engine::core::MeshAssetHeader header{};
  header.magic = engine::core::kMeshAssetMagic;
  header.version = data.hasUVs ? engine::core::kMeshAssetVersion2
                               : engine::core::kMeshAssetVersion;
  header.vertexCount = static_cast<std::uint32_t>(vertexCount);
  header.indexCount = static_cast<std::uint32_t>(data.indices.size());

  if (std::fwrite(&header, sizeof(header), 1U, outputFile) != 1U) {
    std::fprintf(stderr, "error: failed to write mesh header\n");
    std::fclose(outputFile);
    return false;
  }

  if (!data.interleavedVertices.empty()) {
    const std::size_t vertexBytes =
        data.interleavedVertices.size() * sizeof(float);
    if (std::fwrite(
            data.interleavedVertices.data(), 1U, vertexBytes, outputFile)
        != vertexBytes) {
      std::fprintf(stderr, "error: failed to write vertex data\n");
      std::fclose(outputFile);
      return false;
    }
  }

  if (!data.indices.empty()) {
    const std::size_t indexBytes = data.indices.size() * sizeof(std::uint32_t);
    if (std::fwrite(data.indices.data(), 1U, indexBytes, outputFile)
        != indexBytes) {
      std::fprintf(stderr, "error: failed to write index data\n");
      std::fclose(outputFile);
      return false;
    }
  }

  std::fclose(outputFile);
  return true;
}

bool write_metadata_file(const char *inputPath,
                         const char *outputPath,
                         const PrimitiveData &data) {
  if ((inputPath == nullptr) || (outputPath == nullptr)) {
    return false;
  }

  char metadataPath[512] = {};
  const int pathResult = std::snprintf(
      metadataPath, sizeof(metadataPath), "%s.meta.json", outputPath);
  if ((pathResult <= 0)
      || (pathResult >= static_cast<int>(sizeof(metadataPath)))) {
    return false;
  }

  FILE *metadataFile = nullptr;
#ifdef _WIN32
  if (fopen_s(&metadataFile, metadataPath, "wb") != 0) {
    metadataFile = nullptr;
  }
#else
  metadataFile = std::fopen(metadataPath, "wb");
#endif
  if (metadataFile == nullptr) {
    return false;
  }

  const std::size_t vertexCount =
      data.interleavedVertices.size() / (data.hasUVs ? 8U : 6U);
  const std::size_t indexCount = data.indices.size();

  const int written = std::fprintf(
      metadataFile,
      "{\n"
      "  \"schemaVersion\": 1,\n"
      "  \"source\": \"%s\",\n"
      "  \"output\": \"%s\",\n"
      "  \"assetFormat\": \"engine.mesh\",\n"
      "  \"assetFormatVersion\": %u,\n"
      "  \"vertexCount\": %zu,\n"
      "  \"indexCount\": %zu,\n"
      "  \"importSettings\": {\n"
      "    \"meshIndex\": 0,\n"
      "    \"primitiveIndex\": 0,\n"
      "    \"interleavedLayout\": \"%s\"\n"
      "  }\n"
      "}\n",
      inputPath,
      outputPath,
      data.hasUVs ? engine::core::kMeshAssetVersion2
                  : engine::core::kMeshAssetVersion,
      vertexCount,
      indexCount,
      data.hasUVs ? "position_normal_texcoord" : "position_normal");

  std::fclose(metadataFile);
  return written > 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    print_usage();
    return 1;
  }

  const char *inputPath = argv[1];
  const char *outputPath = argv[2];

  cgltf_options options{};
  cgltf_data *data = nullptr;
  const cgltf_result parseResult = cgltf_parse_file(&options, inputPath, &data);
  if ((parseResult != cgltf_result_success) || (data == nullptr)) {
    std::fprintf(stderr, "error: failed to parse glTF file: %s\n", inputPath);
    return 2;
  }

  const cgltf_result loadResult = cgltf_load_buffers(&options, data, inputPath);
  if (loadResult != cgltf_result_success) {
    std::fprintf(stderr, "error: failed to load glTF buffers\n");
    cgltf_free(data);
    return 3;
  }

  if ((data->meshes_count == 0U) || (data->meshes[0].primitives_count == 0U)
      || (data->meshes[0].primitives == nullptr)) {
    std::fprintf(stderr, "error: glTF has no mesh primitives\n");
    cgltf_free(data);
    return 4;
  }

  const cgltf_primitive *primitive = &data->meshes[0].primitives[0];
  PrimitiveData primitiveData{};
  if (!extract_primitive(primitive, &primitiveData)) {
    cgltf_free(data);
    return 5;
  }

  const bool writeOk = write_mesh_file(outputPath, primitiveData);
  cgltf_free(data);

  if (!writeOk) {
    return 6;
  }

  if (!write_metadata_file(inputPath, outputPath, primitiveData)) {
    std::fprintf(stderr, "error: failed to write metadata sidecar\n");
    return 7;
  }

  std::printf(
      "packed mesh: vertices=%zu indices=%zu uvs=%s -> %s (+ .meta.json)\n",
      primitiveData.interleavedVertices.size()
          / (primitiveData.hasUVs ? 8U : 6U),
      primitiveData.indices.size(),
      primitiveData.hasUVs ? "yes" : "no",
      outputPath);
  return 0;
}
