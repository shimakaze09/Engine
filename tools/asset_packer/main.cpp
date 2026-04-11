#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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

struct DependencyDigest final {
  std::string path{};
  std::uint64_t hash = 0ULL;
};

constexpr std::uint64_t kFnv64Offset = 1469598103934665603ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

void print_usage() {
  std::fprintf(stderr,
               "usage: asset_packer <input.gltf|input.glb> <output.mesh> "
               "[--dep <dependency_path>]... [--force]\n");
}

bool file_exists(const char *path) {
  if (path == nullptr) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fclose(file);
  return true;
}

std::uint64_t hash_file_contents(const char *path, bool *ok) {
  if (ok != nullptr) {
    *ok = false;
  }

  if (path == nullptr) {
    return 0ULL;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, path, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path, "rb");
#endif
  if (file == nullptr) {
    return 0ULL;
  }

  std::uint64_t hash = kFnv64Offset;
  unsigned char buffer[4096] = {};
  while (true) {
    const std::size_t bytesRead = std::fread(buffer, 1U, sizeof(buffer), file);
    if (bytesRead == 0U) {
      break;
    }
    for (std::size_t i = 0U; i < bytesRead; ++i) {
      hash ^= static_cast<std::uint64_t>(buffer[i]);
      hash *= kFnv64Prime;
    }
  }

  std::fclose(file);
  if (ok != nullptr) {
    *ok = true;
  }
  return hash;
}

bool build_dependency_digests(const std::vector<std::string> &dependencyPaths,
                              std::vector<DependencyDigest> *outDigests) {
  if (outDigests == nullptr) {
    return false;
  }

  outDigests->clear();
  outDigests->reserve(dependencyPaths.size());
  for (const std::string &path : dependencyPaths) {
    bool ok = false;
    const std::uint64_t hash = hash_file_contents(path.c_str(), &ok);
    if (!ok) {
      std::fprintf(stderr, "error: dependency missing or unreadable: %s\n",
                   path.c_str());
      return false;
    }
    DependencyDigest digest{};
    digest.path = path;
    digest.hash = hash;
    outDigests->push_back(digest);
  }

  return true;
}

bool make_cookstamp_path(const char *outputPath, char *outPath,
                         std::size_t outPathSize) {
  if ((outputPath == nullptr) || (outPath == nullptr) || (outPathSize == 0U)) {
    return false;
  }
  const int written =
      std::snprintf(outPath, outPathSize, "%s.cookstamp", outputPath);
  return (written > 0) && (written < static_cast<int>(outPathSize));
}

bool write_cook_stamp(const char *outputPath, std::uint64_t sourceHash,
                      const std::vector<DependencyDigest> &dependencies) {
  char stampPath[512] = {};
  if (!make_cookstamp_path(outputPath, stampPath, sizeof(stampPath))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, stampPath, "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(stampPath, "wb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fprintf(file, "SCHEMA 1\n");
  std::fprintf(file, "SOURCE_HASH %016llx\n",
               static_cast<unsigned long long>(sourceHash));
  for (const DependencyDigest &dependency : dependencies) {
    std::fprintf(file, "DEP_HASH %016llx %s\n",
                 static_cast<unsigned long long>(dependency.hash),
                 dependency.path.c_str());
  }

  std::fclose(file);
  return true;
}

bool read_cook_stamp(const char *outputPath, std::uint64_t *outSourceHash,
                     std::vector<DependencyDigest> *outDependencies) {
  if ((outSourceHash == nullptr) || (outDependencies == nullptr)) {
    return false;
  }

  char stampPath[512] = {};
  if (!make_cookstamp_path(outputPath, stampPath, sizeof(stampPath))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, stampPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(stampPath, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  *outSourceHash = 0ULL;
  outDependencies->clear();

  char line[1024] = {};
  while (std::fgets(line, static_cast<int>(sizeof(line)), file) != nullptr) {
    unsigned long long hash = 0ULL;
    if (std::sscanf(line, "SOURCE_HASH %llx", &hash) == 1) {
      *outSourceHash = static_cast<std::uint64_t>(hash);
      continue;
    }

    char depPath[900] = {};
    if (std::sscanf(line, "DEP_HASH %llx %899[^\n]", &hash, depPath) == 2) {
      DependencyDigest dep{};
      dep.path = depPath;
      dep.hash = static_cast<std::uint64_t>(hash);
      outDependencies->push_back(dep);
    }
  }

  std::fclose(file);
  return true;
}

bool dependency_digests_equal(const std::vector<DependencyDigest> &a,
                              const std::vector<DependencyDigest> &b) {
  if (a.size() != b.size()) {
    return false;
  }

  for (std::size_t i = 0U; i < a.size(); ++i) {
    if ((a[i].hash != b[i].hash) || (a[i].path != b[i].path)) {
      return false;
    }
  }

  return true;
}

bool should_repack(const char *outputPath, std::uint64_t sourceHash,
                   const std::vector<DependencyDigest> &dependencies) {
  if (!file_exists(outputPath)) {
    return true;
  }

  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  if (!read_cook_stamp(outputPath, &previousSourceHash,
                       &previousDependencies)) {
    return true;
  }

  if (previousSourceHash != sourceHash) {
    return true;
  }

  return !dependency_digests_equal(previousDependencies, dependencies);
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

  if ((positions->type != cgltf_type_vec3) ||
      (normals->type != cgltf_type_vec3)) {
    std::fprintf(stderr, "error: POSITION and NORMAL must be vec3\n");
    return false;
  }

  if (positions->count != normals->count) {
    std::fprintf(stderr,
                 "error: POSITION and NORMAL vertex counts do not match\n");
    return false;
  }

  const bool hasUVs = (texcoords != nullptr) &&
                      (texcoords->type == cgltf_type_vec2) &&
                      (texcoords->count == positions->count);
  outData->hasUVs = hasUVs;
  const std::size_t strideFloats = hasUVs ? 8U : 6U;

  const std::size_t vertexCount = static_cast<std::size_t>(positions->count);
  outData->interleavedVertices.assign(vertexCount * strideFloats, 0.0F);

  std::array<float, 3U> position{};
  std::array<float, 3U> normal{};
  std::array<float, 2U> uv{};

  for (std::size_t i = 0U; i < vertexCount; ++i) {
    if (!cgltf_accessor_read_float(positions, static_cast<cgltf_size>(i),
                                   position.data(), position.size()) ||
        !cgltf_accessor_read_float(normals, static_cast<cgltf_size>(i),
                                   normal.data(), normal.size())) {
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
      if (!cgltf_accessor_read_float(texcoords, static_cast<cgltf_size>(i),
                                     uv.data(), uv.size())) {
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
      const cgltf_uint index = static_cast<cgltf_uint>(cgltf_accessor_read_index(
          primitive->indices, static_cast<cgltf_size>(i)));
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
  if (vertexCount > static_cast<std::size_t>(UINT32_MAX) ||
      (data.indices.size() > static_cast<std::size_t>(UINT32_MAX))) {
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
    if (std::fwrite(data.interleavedVertices.data(), 1U, vertexBytes,
                    outputFile) != vertexBytes) {
      std::fprintf(stderr, "error: failed to write vertex data\n");
      std::fclose(outputFile);
      return false;
    }
  }

  if (!data.indices.empty()) {
    const std::size_t indexBytes = data.indices.size() * sizeof(std::uint32_t);
    if (std::fwrite(data.indices.data(), 1U, indexBytes, outputFile) !=
        indexBytes) {
      std::fprintf(stderr, "error: failed to write index data\n");
      std::fclose(outputFile);
      return false;
    }
  }

  std::fclose(outputFile);
  return true;
}

bool write_metadata_file(const char *inputPath, const char *outputPath,
                         const PrimitiveData &data, std::uint64_t sourceHash,
                         const std::vector<DependencyDigest> &dependencies) {
  if ((inputPath == nullptr) || (outputPath == nullptr)) {
    return false;
  }

  char metadataPath[512] = {};
  const int pathResult = std::snprintf(metadataPath, sizeof(metadataPath),
                                       "%s.meta.json", outputPath);
  if ((pathResult <= 0) ||
      (pathResult >= static_cast<int>(sizeof(metadataPath)))) {
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

  int written = std::fprintf(metadataFile,
                             "{\n"
                             "  \"schemaVersion\": 1,\n"
                             "  \"source\": \"%s\",\n"
                             "  \"output\": \"%s\",\n"
                             "  \"assetFormat\": \"engine.mesh\",\n"
                             "  \"assetFormatVersion\": %u,\n"
                             "  \"sourceContentHash\": \"%016llx\",\n"
                             "  \"vertexCount\": %zu,\n"
                             "  \"indexCount\": %zu,\n"
                             "  \"dependencies\": [\n",
                             inputPath, outputPath,
                             data.hasUVs ? engine::core::kMeshAssetVersion2
                                         : engine::core::kMeshAssetVersion,
                             static_cast<unsigned long long>(sourceHash),
                             vertexCount, indexCount);

  for (std::size_t i = 0U; i < dependencies.size(); ++i) {
    const DependencyDigest &dependency = dependencies[i];
    written += std::fprintf(
        metadataFile, "    { \"path\": \"%s\", \"hash\": \"%016llx\" }%s\n",
        dependency.path.c_str(),
        static_cast<unsigned long long>(dependency.hash),
        (i + 1U < dependencies.size()) ? "," : "");
  }

  written += std::fprintf(metadataFile,
                          "  ],\n"
                          "  \"importSettings\": {\n"
                          "    \"meshIndex\": 0,\n"
                          "    \"primitiveIndex\": 0,\n"
                          "    \"interleavedLayout\": \"%s\"\n"
                          "  }\n"
                          "}\n",
                          data.hasUVs ? "position_normal_texcoord"
                                      : "position_normal");

  std::fclose(metadataFile);
  return written > 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    print_usage();
    return 1;
  }

  const char *inputPath = argv[1];
  const char *outputPath = argv[2];

  bool forceRepack = false;
  std::vector<std::string> dependencyPaths{};
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dep") == 0) {
      if ((i + 1) >= argc) {
        print_usage();
        return 8;
      }
      dependencyPaths.emplace_back(argv[i + 1]);
      ++i;
      continue;
    }

    if (std::strcmp(argv[i], "--force") == 0) {
      forceRepack = true;
      continue;
    }

    print_usage();
    return 9;
  }

  bool sourceHashOk = false;
  const std::uint64_t sourceHash = hash_file_contents(inputPath, &sourceHashOk);
  if (!sourceHashOk) {
    std::fprintf(stderr, "error: failed to read input file for hashing\n");
    return 10;
  }

  std::vector<DependencyDigest> dependencyDigests{};
  if (!build_dependency_digests(dependencyPaths, &dependencyDigests)) {
    return 11;
  }

  if (!forceRepack &&
      !should_repack(outputPath, sourceHash, dependencyDigests)) {
    std::printf("asset up-to-date; skipped recook: %s\n", outputPath);
    return 0;
  }

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

  if ((data->meshes_count == 0U) || (data->meshes[0].primitives_count == 0U) ||
      (data->meshes[0].primitives == nullptr)) {
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

  if (!write_metadata_file(inputPath, outputPath, primitiveData, sourceHash,
                           dependencyDigests)) {
    std::fprintf(stderr, "error: failed to write metadata sidecar\n");
    return 12;
  }

  if (!write_cook_stamp(outputPath, sourceHash, dependencyDigests)) {
    std::fprintf(stderr, "error: failed to write cook stamp\n");
    return 13;
  }

  std::printf(
      "packed mesh: vertices=%zu indices=%zu uvs=%s -> %s (+ .meta.json)\n",
      primitiveData.interleavedVertices.size() /
          (primitiveData.hasUVs ? 8U : 6U),
      primitiveData.indices.size(), primitiveData.hasUVs ? "yes" : "no",
      outputPath);
  return 0;
}
