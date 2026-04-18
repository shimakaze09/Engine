#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // sprintf deprecated
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "engine/core/json.h"
#include "engine/core/mesh_asset.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"

#include "dependency_graph.h"

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

struct ImportSettings final {
  int meshIndex = 0;
  int primitiveIndex = 0;
  float scaleFactor = 1.0F;
  int upAxis = 1;
  bool generateNormals = false;
};

constexpr std::uint64_t kFnv64Offset = 1469598103934665603ULL;
constexpr std::uint64_t kFnv64Prime = 1099511628211ULL;

void print_usage() {
  std::fprintf(stderr,
               "usage: asset_packer <input.gltf|input.glb> <output.mesh> "
               "[--dep <dependency_path>]... [--graph <asset_deps.json>] "
               "[--force]\n");
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

std::uint64_t hash_import_settings(const ImportSettings &settings) {
  std::uint64_t hash = kFnv64Offset;
  auto feed = [&](const void *data, std::size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0U; i < size; ++i) {
      hash ^= static_cast<std::uint64_t>(bytes[i]);
      hash *= kFnv64Prime;
    }
  };
  feed(&settings.meshIndex, sizeof(settings.meshIndex));
  feed(&settings.primitiveIndex, sizeof(settings.primitiveIndex));
  feed(&settings.scaleFactor, sizeof(settings.scaleFactor));
  feed(&settings.upAxis, sizeof(settings.upAxis));
  feed(&settings.generateNormals, sizeof(settings.generateNormals));
  return hash;
}

void sort_dependency_digests(std::vector<DependencyDigest> &digests) {
  std::sort(digests.begin(), digests.end(),
            [](const DependencyDigest &a, const DependencyDigest &b) {
              return a.path < b.path;
            });
}

bool read_import_settings_from_meta(const char *outputPath,
                                    ImportSettings *outSettings) {
  if ((outputPath == nullptr) || (outSettings == nullptr)) {
    return false;
  }

  char metadataPath[512] = {};
  const int pathResult = std::snprintf(metadataPath, sizeof(metadataPath),
                                       "%s.meta.json", outputPath);
  if ((pathResult <= 0) ||
      (pathResult >= static_cast<int>(sizeof(metadataPath)))) {
    return false;
  }

  FILE *file = nullptr;
#ifdef _WIN32
  if (fopen_s(&file, metadataPath, "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(metadataPath, "rb");
#endif
  if (file == nullptr) {
    return false;
  }

  std::fseek(file, 0, SEEK_END);
  const long fileSize = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (fileSize <= 0 || fileSize > 1024 * 1024) {
    std::fclose(file);
    return false;
  }

  std::vector<char> buffer(static_cast<std::size_t>(fileSize) + 1U, '\0');
  const std::size_t readBytes =
      std::fread(buffer.data(), 1U, static_cast<std::size_t>(fileSize), file);
  std::fclose(file);
  if (readBytes == 0U) {
    return false;
  }

  engine::core::JsonParser parser{};
  if (!parser.parse(buffer.data(), readBytes)) {
    return false;
  }

  const engine::core::JsonValue *root = parser.root();
  if ((root == nullptr) ||
      (root->type != engine::core::JsonValue::Type::Object)) {
    return false;
  }

  const engine::core::JsonValue *importObj =
      parser.get_object_field(*root, "importSettings");
  if ((importObj == nullptr) ||
      (importObj->type != engine::core::JsonValue::Type::Object)) {
    return false;
  }

  float scaleFactor = 1.0F;
  std::uint32_t meshIndex = 0U;
  std::uint32_t primitiveIndex = 0U;
  std::uint32_t upAxis = 1U;
  bool generateNormals = false;

  const engine::core::JsonValue *scaleVal =
      parser.get_object_field(*importObj, "scaleFactor");
  if (scaleVal != nullptr) {
    parser.as_float(*scaleVal, &scaleFactor);
  }

  const engine::core::JsonValue *meshVal =
      parser.get_object_field(*importObj, "meshIndex");
  if (meshVal != nullptr) {
    parser.as_uint(*meshVal, &meshIndex);
  }

  const engine::core::JsonValue *primVal =
      parser.get_object_field(*importObj, "primitiveIndex");
  if (primVal != nullptr) {
    parser.as_uint(*primVal, &primitiveIndex);
  }

  const engine::core::JsonValue *upVal =
      parser.get_object_field(*importObj, "upAxis");
  if (upVal != nullptr) {
    parser.as_uint(*upVal, &upAxis);
  }

  const engine::core::JsonValue *normVal =
      parser.get_object_field(*importObj, "generateNormals");
  if (normVal != nullptr) {
    parser.as_bool(*normVal, &generateNormals);
  }

  outSettings->scaleFactor = scaleFactor;
  outSettings->meshIndex = static_cast<int>(meshIndex);
  outSettings->primitiveIndex = static_cast<int>(primitiveIndex);
  outSettings->upAxis = static_cast<int>(upAxis);
  outSettings->generateNormals = generateNormals;
  return true;
}

void apply_scale_to_primitive(PrimitiveData *data, float scaleFactor) {
  if ((data == nullptr) || (scaleFactor == 1.0F)) {
    return;
  }

  const std::size_t strideFloats = data->hasUVs ? 8U : 6U;
  const std::size_t vertexCount =
      data->interleavedVertices.size() / strideFloats;
  for (std::size_t i = 0U; i < vertexCount; ++i) {
    const std::size_t base = i * strideFloats;
    data->interleavedVertices[base + 0U] *= scaleFactor;
    data->interleavedVertices[base + 1U] *= scaleFactor;
    data->interleavedVertices[base + 2U] *= scaleFactor;
    // Normals are direction vectors — do not scale.
  }
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
                      const std::vector<DependencyDigest> &dependencies,
                      std::uint64_t importSettingsHash) {
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

  std::fprintf(file, "SCHEMA 2\n");
  std::fprintf(file, "SOURCE_HASH %016llx\n",
               static_cast<unsigned long long>(sourceHash));
  std::fprintf(file, "IMPORT_HASH %016llx\n",
               static_cast<unsigned long long>(importSettingsHash));
  for (const DependencyDigest &dependency : dependencies) {
    std::fprintf(file, "DEP_HASH %016llx %s\n",
                 static_cast<unsigned long long>(dependency.hash),
                 dependency.path.c_str());
  }

  std::fclose(file);
  return true;
}

bool read_cook_stamp(const char *outputPath, std::uint64_t *outSourceHash,
                     std::vector<DependencyDigest> *outDependencies,
                     std::uint64_t *outImportSettingsHash) {
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
  if (outImportSettingsHash != nullptr) {
    *outImportSettingsHash = 0ULL;
  }

  char line[1024] = {};
  while (std::fgets(line, static_cast<int>(sizeof(line)), file) != nullptr) {
    unsigned long long hash = 0ULL;
    if (std::sscanf(line, "SOURCE_HASH %llx", &hash) == 1) {
      *outSourceHash = static_cast<std::uint64_t>(hash);
      continue;
    }

    if (std::sscanf(line, "IMPORT_HASH %llx", &hash) == 1) {
      if (outImportSettingsHash != nullptr) {
        *outImportSettingsHash = static_cast<std::uint64_t>(hash);
      }
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
                   const std::vector<DependencyDigest> &dependencies,
                   std::uint64_t importSettingsHash) {
  if (!file_exists(outputPath)) {
    return true;
  }

  std::uint64_t previousSourceHash = 0ULL;
  std::vector<DependencyDigest> previousDependencies{};
  std::uint64_t previousImportHash = 0ULL;
  if (!read_cook_stamp(outputPath, &previousSourceHash, &previousDependencies,
                       &previousImportHash)) {
    return true;
  }

  if (previousSourceHash != sourceHash) {
    return true;
  }

  if (previousImportHash != importSettingsHash) {
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
      const cgltf_uint index =
          static_cast<cgltf_uint>(cgltf_accessor_read_index(
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
                         const std::vector<DependencyDigest> &dependencies,
                         const ImportSettings &importSettings) {
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

  // Compute output file size.
  std::uint64_t outputFileSize = 0ULL;
  {
    FILE *outputCheck = nullptr;
#ifdef _WIN32
    if (fopen_s(&outputCheck, outputPath, "rb") != 0) {
      outputCheck = nullptr;
    }
#else
    outputCheck = std::fopen(outputPath, "rb");
#endif
    if (outputCheck != nullptr) {
      std::fseek(outputCheck, 0, SEEK_END);
      outputFileSize = static_cast<std::uint64_t>(std::ftell(outputCheck));
      std::fclose(outputCheck);
    }
  }

  int written = std::fprintf(
      metadataFile,
      "{\n"
      "  \"schemaVersion\": 2,\n"
      "  \"assetId\": \"%016llx\",\n"
      "  \"typeTag\": \"mesh\",\n"
      "  \"source\": \"%s\",\n"
      "  \"output\": \"%s\",\n"
      "  \"assetFormat\": \"engine.mesh\",\n"
      "  \"assetFormatVersion\": %u,\n"
      "  \"sourceContentHash\": \"%016llx\",\n"
      "  \"fileSize\": %llu,\n"
      "  \"vertexCount\": %zu,\n"
      "  \"indexCount\": %zu,\n"
      "  \"tags\": [],\n"
      "  \"dependencies\": [\n",
      static_cast<unsigned long long>(sourceHash), inputPath, outputPath,
      data.hasUVs ? engine::core::kMeshAssetVersion2
                  : engine::core::kMeshAssetVersion,
      static_cast<unsigned long long>(sourceHash),
      static_cast<unsigned long long>(outputFileSize), vertexCount, indexCount);

  for (std::size_t i = 0U; i < dependencies.size(); ++i) {
    const DependencyDigest &dependency = dependencies[i];
    written += std::fprintf(
        metadataFile, "    { \"path\": \"%s\", \"hash\": \"%016llx\" }%s\n",
        dependency.path.c_str(),
        static_cast<unsigned long long>(dependency.hash),
        (i + 1U < dependencies.size()) ? "," : "");
  }

  written += std::fprintf(
      metadataFile,
      "  ],\n"
      "  \"importSettings\": {\n"
      "    \"meshIndex\": %d,\n"
      "    \"primitiveIndex\": %d,\n"
      "    \"scaleFactor\": %.6g,\n"
      "    \"upAxis\": %d,\n"
      "    \"generateNormals\": %s,\n"
      "    \"interleavedLayout\": \"%s\"\n"
      "  }\n"
      "}\n",
      importSettings.meshIndex, importSettings.primitiveIndex,
      static_cast<double>(importSettings.scaleFactor), importSettings.upAxis,
      importSettings.generateNormals ? "true" : "false",
      data.hasUVs ? "position_normal_texcoord" : "position_normal");

  std::fclose(metadataFile);
  return written > 0;
}

bool cook_and_write_convex_hull(const char *outputPath,
                                const PrimitiveData &data) {
  if (outputPath == nullptr) {
    return false;
  }

  const std::size_t strideFloats = data.hasUVs ? 8U : 6U;
  const std::size_t vertexCount =
      data.interleavedVertices.size() / strideFloats;
  if (vertexCount < 4U) {
    std::fprintf(stderr, "warning: too few vertices (%zu) for convex hull\n",
                 vertexCount);
    return false;
  }

  // Extract positions from interleaved data.
  std::vector<engine::math::Vec3> positions(vertexCount);
  for (std::size_t i = 0U; i < vertexCount; ++i) {
    const std::size_t base = i * strideFloats;
    positions[i] = engine::math::Vec3(data.interleavedVertices[base + 0U],
                                      data.interleavedVertices[base + 1U],
                                      data.interleavedVertices[base + 2U]);
  }

  engine::physics::ConvexHullData hull{};
  if (!engine::physics::build_convex_hull(positions.data(), vertexCount,
                                          hull)) {
    std::fprintf(stderr, "warning: convex hull build failed\n");
    return false;
  }

  // Write hull sidecar: <output>.hull (binary).
  char hullPath[512] = {};
  const int pathLen =
      std::snprintf(hullPath, sizeof(hullPath), "%s.hull", outputPath);
  if ((pathLen <= 0) || (pathLen >= static_cast<int>(sizeof(hullPath)))) {
    return false;
  }

  FILE *hullFile = nullptr;
#ifdef _WIN32
  if (fopen_s(&hullFile, hullPath, "wb") != 0) {
    hullFile = nullptr;
  }
#else
  hullFile = std::fopen(hullPath, "wb");
#endif
  if (hullFile == nullptr) {
    std::fprintf(stderr, "error: failed to open hull file: %s\n", hullPath);
    return false;
  }

  // Header: magic (4 bytes) + planeCount (4) + vertexCount (4) + localCenter
  // (12) + localHalfExtents (12) = 36 bytes.
  constexpr std::uint32_t kHullMagic = 0x48554C4CU; // 'HULL'
  const std::uint32_t planeCount32 =
      static_cast<std::uint32_t>(hull.planeCount);
  const std::uint32_t vertCount32 =
      static_cast<std::uint32_t>(hull.vertexCount);

  bool ok = true;
  ok = ok && (std::fwrite(&kHullMagic, 4U, 1U, hullFile) == 1U);
  ok = ok && (std::fwrite(&planeCount32, 4U, 1U, hullFile) == 1U);
  ok = ok && (std::fwrite(&vertCount32, 4U, 1U, hullFile) == 1U);
  ok =
      ok && (std::fwrite(&hull.localCenter, sizeof(float), 3U, hullFile) == 3U);
  ok = ok &&
       (std::fwrite(&hull.localHalfExtents, sizeof(float), 3U, hullFile) == 3U);

  // Planes: each is (normal.x, normal.y, normal.z, distance) = 16 bytes.
  for (std::size_t i = 0U; i < hull.planeCount && ok; ++i) {
    ok = ok && (std::fwrite(&hull.planes[i].normal, sizeof(float), 3U,
                            hullFile) == 3U);
    ok = ok && (std::fwrite(&hull.planes[i].distance, sizeof(float), 1U,
                            hullFile) == 1U);
  }

  // Vertices: each is (x, y, z) = 12 bytes.
  for (std::size_t i = 0U; i < hull.vertexCount && ok; ++i) {
    ok = ok &&
         (std::fwrite(&hull.vertices[i], sizeof(float), 3U, hullFile) == 3U);
  }

  std::fclose(hullFile);
  if (!ok) {
    std::fprintf(stderr, "error: failed to write hull data\n");
    return false;
  }

  std::printf("cooked convex hull: planes=%u vertices=%u -> %s\n", planeCount32,
              vertCount32, hullPath);
  return true;
}

bool ensure_directory_exists(const char *dirPath) {
  if (dirPath == nullptr) {
    return false;
  }

#ifdef _WIN32
  // CreateDirectoryA returns 0 if it fails; ERROR_ALREADY_EXISTS is OK.
  // Use _mkdir from direct.h as a simpler portable option.
  struct _stat st{};
  if (_stat(dirPath, &st) == 0) {
    return true;
  }
  return _mkdir(dirPath) == 0;
#else
  struct stat st{};
  if (stat(dirPath, &st) == 0) {
    return true;
  }
  return mkdir(dirPath, 0755) == 0;
#endif
}

// Returns file modification time in seconds, or 0 on failure.
std::int64_t get_file_mtime(const char *path) noexcept {
  if (path == nullptr) {
    return 0;
  }
#ifdef _WIN32
  struct _stat st{};
  if (_stat(path, &st) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(st.st_mtime);
#else
  struct stat st{};
  if (stat(path, &st) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(st.st_mtime);
#endif
}

// Build thumbnail path: <dir>/.thumbnails/<basename>.png
void build_thumbnail_path(const char *outputPath, char *thumbPath,
                          std::size_t thumbPathSize) noexcept {
  const char *lastSlash = std::strrchr(outputPath, '/');
  const char *lastBackSlash = std::strrchr(outputPath, '\\');
  if ((lastBackSlash != nullptr) &&
      ((lastSlash == nullptr) || (lastBackSlash > lastSlash))) {
    lastSlash = lastBackSlash;
  }

  char thumbDir[512] = {};
  if (lastSlash != nullptr) {
    const std::size_t dirLen = static_cast<std::size_t>(lastSlash - outputPath);
    if (dirLen < sizeof(thumbDir) - 13U) {
      std::memcpy(thumbDir, outputPath, dirLen);
      std::snprintf(thumbDir + dirLen, sizeof(thumbDir) - dirLen,
                    "/.thumbnails");
    } else {
      std::snprintf(thumbDir, sizeof(thumbDir), ".thumbnails");
    }
  } else {
    std::snprintf(thumbDir, sizeof(thumbDir), ".thumbnails");
  }
  ensure_directory_exists(thumbDir);

  const char *basename = (lastSlash != nullptr) ? (lastSlash + 1) : outputPath;
  std::snprintf(thumbPath, thumbPathSize, "%s/%s.png", thumbDir, basename);
}

bool generate_mesh_thumbnail(const char *inputPath, const char *outputPath,
                             const PrimitiveData &data) {
  if (outputPath == nullptr) {
    return false;
  }

  // Build thumbnail output path.
  char thumbPath[512] = {};
  build_thumbnail_path(outputPath, thumbPath, sizeof(thumbPath));

  // Mtime invalidation: skip if thumbnail is newer than source.
  if (inputPath != nullptr) {
    const std::int64_t srcMtime = get_file_mtime(inputPath);
    const std::int64_t thumbMtime = get_file_mtime(thumbPath);
    if ((thumbMtime > 0) && (srcMtime > 0) && (thumbMtime >= srcMtime)) {
      std::printf("thumbnail up-to-date; skipped: %s\n", thumbPath);
      return true;
    }
  }

  constexpr int kThumbSize = 64;
  constexpr int kChannels = 4; // RGBA
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(kThumbSize * kThumbSize * kChannels), 0U);
  std::vector<float> depth(static_cast<std::size_t>(kThumbSize * kThumbSize),
                           1e30F);

  const std::size_t strideFloats = data.hasUVs ? 8U : 6U;
  const std::size_t vertexCount =
      data.interleavedVertices.size() / strideFloats;
  if (vertexCount == 0U) {
    return false;
  }

  // Compute AABB.
  float minX = 1e30F;
  float minY = 1e30F;
  float minZ = 1e30F;
  float maxX = -1e30F;
  float maxY = -1e30F;
  float maxZ = -1e30F;
  for (std::size_t i = 0U; i < vertexCount; ++i) {
    const std::size_t base = i * strideFloats;
    const float x = data.interleavedVertices[base + 0U];
    const float y = data.interleavedVertices[base + 1U];
    const float z = data.interleavedVertices[base + 2U];
    if (x < minX) {
      minX = x;
    }
    if (y < minY) {
      minY = y;
    }
    if (z < minZ) {
      minZ = z;
    }
    if (x > maxX) {
      maxX = x;
    }
    if (y > maxY) {
      maxY = y;
    }
    if (z > maxZ) {
      maxZ = z;
    }
  }

  const float cx = (minX + maxX) * 0.5F;
  const float cy = (minY + maxY) * 0.5F;
  const float dx = maxX - minX;
  const float dy = maxY - minY;
  const float dz = maxZ - minZ;
  float extent = dx;
  if (dy > extent) {
    extent = dy;
  }
  if (dz > extent) {
    extent = dz;
  }
  if (extent < 1e-6F) {
    extent = 1.0F;
  }
  const float invExtent = static_cast<float>(kThumbSize - 4) / extent;

  // Simple orthographic projection from +Z looking at center.
  // Light direction: normalized (0.5, 0.7, 1.0).
  constexpr float kLightX = 0.365148F;
  constexpr float kLightY = 0.511208F;
  constexpr float kLightZ = 0.730297F;

  // Rasterize triangles.
  auto project = [&](std::size_t vi, float *sx, float *sy, float *sz) {
    const std::size_t base = vi * strideFloats;
    const float x = data.interleavedVertices[base + 0U];
    const float y = data.interleavedVertices[base + 1U];
    const float z = data.interleavedVertices[base + 2U];
    *sx = (x - cx) * invExtent + static_cast<float>(kThumbSize) * 0.5F;
    *sy = static_cast<float>(kThumbSize) * 0.5F - (y - cy) * invExtent;
    *sz = z;
  };

  auto rasterizeTriangle = [&](std::size_t i0, std::size_t i1, std::size_t i2) {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float z0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float z1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    float z2 = 0.0F;
    project(i0, &x0, &y0, &z0);
    project(i1, &x1, &y1, &z1);
    project(i2, &x2, &y2, &z2);

    // Face normal from vertex normals (average).
    const std::size_t b0 = i0 * strideFloats;
    const std::size_t b1 = i1 * strideFloats;
    const std::size_t b2 = i2 * strideFloats;
    const float nx =
        (data.interleavedVertices[b0 + 3U] + data.interleavedVertices[b1 + 3U] +
         data.interleavedVertices[b2 + 3U]) /
        3.0F;
    const float ny =
        (data.interleavedVertices[b0 + 4U] + data.interleavedVertices[b1 + 4U] +
         data.interleavedVertices[b2 + 4U]) /
        3.0F;
    const float nz =
        (data.interleavedVertices[b0 + 5U] + data.interleavedVertices[b1 + 5U] +
         data.interleavedVertices[b2 + 5U]) /
        3.0F;
    float dot = nx * kLightX + ny * kLightY + nz * kLightZ;
    if (dot < 0.0F) {
      dot = 0.0F;
    }
    const float ambient = 0.15F;
    const float shade = ambient + (1.0F - ambient) * dot;
    const auto color = static_cast<std::uint8_t>(
        shade > 1.0F ? 255U : static_cast<unsigned>(shade * 255.0F));

    // Bounding box of triangle in screen space.
    float fminX = x0;
    if (x1 < fminX) {
      fminX = x1;
    }
    if (x2 < fminX) {
      fminX = x2;
    }
    float fmaxX = x0;
    if (x1 > fmaxX) {
      fmaxX = x1;
    }
    if (x2 > fmaxX) {
      fmaxX = x2;
    }
    float fminY = y0;
    if (y1 < fminY) {
      fminY = y1;
    }
    if (y2 < fminY) {
      fminY = y2;
    }
    float fmaxY = y0;
    if (y1 > fmaxY) {
      fmaxY = y1;
    }
    if (y2 > fmaxY) {
      fmaxY = y2;
    }

    const int ixMin = static_cast<int>(fminX);
    const int ixMax = static_cast<int>(fmaxX) + 1;
    const int iyMin = static_cast<int>(fminY);
    const int iyMax = static_cast<int>(fmaxY) + 1;

    for (int py = iyMin; py <= iyMax; ++py) {
      if ((py < 0) || (py >= kThumbSize)) {
        continue;
      }
      for (int px = ixMin; px <= ixMax; ++px) {
        if ((px < 0) || (px >= kThumbSize)) {
          continue;
        }
        const float pxf = static_cast<float>(px) + 0.5F;
        const float pyf = static_cast<float>(py) + 0.5F;
        // Barycentric coordinates.
        const float d00 = (x1 - x0) * (pyf - y0) - (y1 - y0) * (pxf - x0);
        const float d01 = (x2 - x1) * (pyf - y1) - (y2 - y1) * (pxf - x1);
        const float d02 = (x0 - x2) * (pyf - y2) - (y0 - y2) * (pxf - x2);
        if ((d00 >= 0.0F && d01 >= 0.0F && d02 >= 0.0F) ||
            (d00 <= 0.0F && d01 <= 0.0F && d02 <= 0.0F)) {
          // Interpolate depth.
          const float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
          float z = z0;
          if (std::fabs(area) > 1e-8F) {
            const float invArea = 1.0F / area;
            const float w0 =
                ((x1 - pxf) * (y2 - pyf) - (y1 - pyf) * (x2 - pxf)) * invArea;
            const float w1 =
                ((x2 - pxf) * (y0 - pyf) - (y2 - pyf) * (x0 - pxf)) * invArea;
            const float w2 = 1.0F - w0 - w1;
            z = w0 * z0 + w1 * z1 + w2 * z2;
          }
          const std::size_t idx =
              static_cast<std::size_t>(py * kThumbSize + px);
          if (z < depth[idx]) {
            depth[idx] = z;
            const std::size_t pi = idx * kChannels;
            pixels[pi + 0U] = color;
            pixels[pi + 1U] = color;
            pixels[pi + 2U] = color;
            pixels[pi + 3U] = 255U;
          }
        }
      }
    }
  };

  if (!data.indices.empty()) {
    for (std::size_t i = 0U; i + 2U < data.indices.size(); i += 3U) {
      rasterizeTriangle(data.indices[i + 0U], data.indices[i + 1U],
                        data.indices[i + 2U]);
    }
  } else {
    for (std::size_t i = 0U; i + 2U < vertexCount; i += 3U) {
      rasterizeTriangle(i + 0U, i + 1U, i + 2U);
    }
  }

  // Set background to a neutral grey with alpha=0 for transparent areas.
  for (std::size_t i = 0U;
       i < static_cast<std::size_t>(kThumbSize * kThumbSize); ++i) {
    if (pixels[i * kChannels + 3U] == 0U) {
      pixels[i * kChannels + 0U] = 48U;
      pixels[i * kChannels + 1U] = 48U;
      pixels[i * kChannels + 2U] = 48U;
      pixels[i * kChannels + 3U] = 255U;
    }
  }

  if (!stbi_write_png(thumbPath, kThumbSize, kThumbSize, kChannels,
                      pixels.data(), kThumbSize * kChannels)) {
    std::fprintf(stderr, "warning: failed to write thumbnail: %s\n", thumbPath);
    return false;
  }

  std::printf("generated thumbnail: %s\n", thumbPath);
  return true;
}

std::uint64_t hash_path_to_asset_id(const char *path) {
  if (path == nullptr) {
    return 0ULL;
  }

  std::uint64_t hash = kFnv64Offset;
  for (const unsigned char *cursor =
           reinterpret_cast<const unsigned char *>(path);
       *cursor != 0U; ++cursor) {
    const unsigned char ch = (*cursor == static_cast<unsigned char>('\\'))
                                 ? static_cast<unsigned char>('/')
                                 : *cursor;
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnv64Prime;
  }

  if (hash == 0ULL) {
    hash = 1ULL;
  }
  return hash;
}

/// Resolve a glTF image URI relative to the input file directory.
bool resolve_image_path(const char *inputPath, const char *imageUri,
                        char *outPath, std::size_t outPathSize) {
  if ((inputPath == nullptr) || (imageUri == nullptr) || (outPath == nullptr) ||
      (outPathSize == 0U)) {
    return false;
  }

  // Find directory of input file.
  const char *lastSlash = std::strrchr(inputPath, '/');
  const char *lastBackslash = std::strrchr(inputPath, '\\');
  const char *sep = lastSlash;
  if ((lastBackslash != nullptr) &&
      ((sep == nullptr) || (lastBackslash > sep))) {
    sep = lastBackslash;
  }

  if (sep != nullptr) {
    const std::size_t dirLen = static_cast<std::size_t>(sep - inputPath) + 1U;
    const std::size_t uriLen = std::strlen(imageUri);
    if ((dirLen + uriLen) >= outPathSize) {
      return false;
    }
    std::memcpy(outPath, inputPath, dirLen);
    std::memcpy(outPath + dirLen, imageUri, uriLen);
    outPath[dirLen + uriLen] = '\0';
  } else {
    const std::size_t uriLen = std::strlen(imageUri);
    if (uriLen >= outPathSize) {
      return false;
    }
    std::memcpy(outPath, imageUri, uriLen);
    outPath[uriLen] = '\0';
  }
  return true;
}

/// Extract texture/material dependencies from glTF data and register them
/// in the dependency graph. Returns dependency paths for cookstamp hashing.
void extract_gltf_dependencies(const cgltf_data *data, const char *inputPath,
                               std::uint64_t meshAssetId,
                               engine::tools::DependencyGraph *graph,
                               std::vector<DependencyDigest> *autoDepDigests) {
  if ((data == nullptr) || (inputPath == nullptr) || (graph == nullptr)) {
    return;
  }

  // Walk all materials used by this mesh's primitives.
  std::unordered_set<const cgltf_image *> seenImages{};

  auto processTexture = [&](const cgltf_texture_view &texView) {
    if ((texView.texture == nullptr) || (texView.texture->image == nullptr)) {
      return;
    }
    const cgltf_image *image = texView.texture->image;
    if (image->uri == nullptr) {
      return; // Embedded texture (buffer view), no external dep.
    }
    if (!seenImages.insert(image).second) {
      return; // Already processed.
    }

    char resolvedPath[512] = {};
    if (!resolve_image_path(inputPath, image->uri, resolvedPath,
                            sizeof(resolvedPath))) {
      return;
    }

    const std::uint64_t texAssetId = hash_path_to_asset_id(resolvedPath);
    if (texAssetId == 0ULL) {
      return;
    }

    engine::tools::register_asset_path(graph, texAssetId, resolvedPath);
    // mesh depends on texture (forward edge).
    // Use direct insert to avoid cycle check for texture→mesh (impossible).
    graph->dependencies[meshAssetId].insert(texAssetId);
    graph->dependents[texAssetId].insert(meshAssetId);

    // Also add to the auto-discovered dependency list for cookstamp.
    if (autoDepDigests != nullptr) {
      bool hashOk = false;
      const std::uint64_t fileHash = hash_file_contents(resolvedPath, &hashOk);
      if (hashOk) {
        DependencyDigest digest{};
        digest.path = resolvedPath;
        digest.hash = fileHash;
        autoDepDigests->push_back(digest);
      }
    }
  };

  for (cgltf_size mi = 0U; mi < data->meshes_count; ++mi) {
    const cgltf_mesh &mesh = data->meshes[mi];
    for (cgltf_size pi = 0U; pi < mesh.primitives_count; ++pi) {
      const cgltf_primitive &prim = mesh.primitives[pi];
      if (prim.material == nullptr) {
        continue;
      }
      const cgltf_material &mat = *prim.material;

      // Register the material as a dependency of the mesh.
      char matName[512] = {};
      if (mat.name != nullptr) {
        std::snprintf(matName, sizeof(matName), "%s#material:%s", inputPath,
                      mat.name);
      } else {
        std::snprintf(matName, sizeof(matName), "%s#material:%zu", inputPath,
                      static_cast<std::size_t>(mi * 1000U + pi));
      }
      const std::uint64_t matAssetId = hash_path_to_asset_id(matName);
      if (matAssetId != 0ULL) {
        engine::tools::register_asset_path(graph, matAssetId, matName);
        graph->dependencies[meshAssetId].insert(matAssetId);
        graph->dependents[matAssetId].insert(meshAssetId);
      }

      // PBR metallic roughness textures.
      if (mat.has_pbr_metallic_roughness) {
        processTexture(mat.pbr_metallic_roughness.base_color_texture);
        processTexture(mat.pbr_metallic_roughness.metallic_roughness_texture);
      }

      // Other textures.
      processTexture(mat.normal_texture);
      processTexture(mat.occlusion_texture);
      processTexture(mat.emissive_texture);
    }
  }
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
  std::string graphPath{};
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

    if (std::strcmp(argv[i], "--graph") == 0) {
      if ((i + 1) >= argc) {
        print_usage();
        return 8;
      }
      graphPath = argv[i + 1];
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

  // Load the dependency graph (if a graph path was provided).
  engine::tools::DependencyGraph depGraph{};
  const bool hasGraphPath = !graphPath.empty();
  if (hasGraphPath) {
    // Load existing graph; failure is ok (first run).
    engine::tools::read_dependency_graph_json(&depGraph, graphPath.c_str());
  }

  // Compute the mesh asset ID for graph registration.
  const std::uint64_t meshAssetId = hash_path_to_asset_id(inputPath);
  if (hasGraphPath && (meshAssetId != 0ULL)) {
    engine::tools::register_asset_path(&depGraph, meshAssetId, inputPath);

    // Check if any dependency in the graph has changed, requiring a repack.
    // This extends beyond just the explicit --dep flags.
    engine::tools::DependencyGraph::AssetId depIds[64] = {};
    const std::size_t depCount =
        engine::tools::get_dependencies(&depGraph, meshAssetId, depIds, 64U);
    for (std::size_t i = 0U; i < depCount; ++i) {
      auto pathIt = depGraph.assetPaths.find(depIds[i]);
      if (pathIt != depGraph.assetPaths.end()) {
        // Check if already in the manual deps list.
        bool alreadyTracked = false;
        for (const auto &d : dependencyDigests) {
          if (d.path == pathIt->second) {
            alreadyTracked = true;
            break;
          }
        }
        if (!alreadyTracked && file_exists(pathIt->second.c_str())) {
          bool hashOk = false;
          const std::uint64_t h =
              hash_file_contents(pathIt->second.c_str(), &hashOk);
          if (hashOk) {
            DependencyDigest d{};
            d.path = pathIt->second;
            d.hash = h;
            dependencyDigests.push_back(d);
          }
        }
      }
    }
  }

  // Read import settings from existing .meta.json (if present).
  ImportSettings importSettings{};
  read_import_settings_from_meta(outputPath, &importSettings);
  const std::uint64_t importSettingsHash = hash_import_settings(importSettings);

  // Sort dependencies by path for deterministic output.
  sort_dependency_digests(dependencyDigests);

  if (!forceRepack && !should_repack(outputPath, sourceHash, dependencyDigests,
                                     importSettingsHash)) {
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

  // Select mesh and primitive from import settings (bounds-checked).
  const cgltf_size meshIdx =
      (importSettings.meshIndex >= 0 &&
       static_cast<cgltf_size>(importSettings.meshIndex) < data->meshes_count)
          ? static_cast<cgltf_size>(importSettings.meshIndex)
          : 0U;
  const cgltf_mesh &selectedMesh = data->meshes[meshIdx];
  const cgltf_size primIdx =
      (importSettings.primitiveIndex >= 0 &&
       static_cast<cgltf_size>(importSettings.primitiveIndex) <
           selectedMesh.primitives_count)
          ? static_cast<cgltf_size>(importSettings.primitiveIndex)
          : 0U;

  const cgltf_primitive *primitive = &selectedMesh.primitives[primIdx];
  PrimitiveData primitiveData{};
  if (!extract_primitive(primitive, &primitiveData)) {
    cgltf_free(data);
    return 5;
  }

  // Apply import settings: scale factor.
  apply_scale_to_primitive(&primitiveData, importSettings.scaleFactor);

  // Extract material/texture dependencies from glTF and register in graph.
  std::vector<DependencyDigest> autoDiscoveredDeps{};
  if (hasGraphPath && (meshAssetId != 0ULL)) {
    // Clear previous forward edges for this asset before repopulating.
    auto fwdIt = depGraph.dependencies.find(meshAssetId);
    if (fwdIt != depGraph.dependencies.end()) {
      for (const auto oldDep : fwdIt->second) {
        auto revIt = depGraph.dependents.find(oldDep);
        if (revIt != depGraph.dependents.end()) {
          revIt->second.erase(meshAssetId);
          if (revIt->second.empty()) {
            depGraph.dependents.erase(revIt);
          }
        }
      }
      depGraph.dependencies.erase(fwdIt);
    }

    extract_gltf_dependencies(data, inputPath, meshAssetId, &depGraph,
                              &autoDiscoveredDeps);

    // Also register manually specified deps in the graph.
    for (const auto &manualDep : dependencyDigests) {
      const std::uint64_t depId = hash_path_to_asset_id(manualDep.path.c_str());
      if (depId != 0ULL) {
        engine::tools::register_asset_path(&depGraph, depId,
                                           manualDep.path.c_str());
        depGraph.dependencies[meshAssetId].insert(depId);
        depGraph.dependents[depId].insert(meshAssetId);
      }
    }
  }

  // Merge auto-discovered deps into the cookstamp dependency list.
  for (const auto &autoDep : autoDiscoveredDeps) {
    bool alreadyPresent = false;
    for (const auto &existing : dependencyDigests) {
      if (existing.path == autoDep.path) {
        alreadyPresent = true;
        break;
      }
    }
    if (!alreadyPresent) {
      dependencyDigests.push_back(autoDep);
    }
  }

  // Sort all dependencies by path for deterministic output.
  sort_dependency_digests(dependencyDigests);

  const bool writeOk = write_mesh_file(outputPath, primitiveData);
  cgltf_free(data);

  if (!writeOk) {
    return 6;
  }

  if (!write_metadata_file(inputPath, outputPath, primitiveData, sourceHash,
                           dependencyDigests, importSettings)) {
    std::fprintf(stderr, "error: failed to write metadata sidecar\n");
    return 12;
  }

  if (!write_cook_stamp(outputPath, sourceHash, dependencyDigests,
                        importSettingsHash)) {
    std::fprintf(stderr, "error: failed to write cook stamp\n");
    return 13;
  }

  // Cook convex hull sidecar (best-effort; failure is non-fatal).
  cook_and_write_convex_hull(outputPath, primitiveData);

  // Generate mesh thumbnail (best-effort; failure is non-fatal).
  generate_mesh_thumbnail(inputPath, outputPath, primitiveData);

  // Save the dependency graph (if graph path was provided).
  if (hasGraphPath) {
    if (!engine::tools::write_dependency_graph_json(&depGraph,
                                                    graphPath.c_str())) {
      std::fprintf(stderr, "warning: failed to write dependency graph: %s\n",
                   graphPath.c_str());
    }
  }

  std::printf(
      "packed mesh: vertices=%zu indices=%zu uvs=%s -> %s (+ .meta.json)\n",
      primitiveData.interleavedVertices.size() /
          (primitiveData.hasUVs ? 8U : 6U),
      primitiveData.indices.size(), primitiveData.hasUVs ? "yes" : "no",
      outputPath);
  return 0;
}
