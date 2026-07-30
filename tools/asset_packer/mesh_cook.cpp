// Implements glTF mesh cooking: primitive extraction, mesh/metadata file
// writing, collision hull cooking, and dependency collection.

#include "packer_shared.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/core/json.h"
#include "engine/core/mesh_asset.h"
#include "engine/math/vec3.h"
#include "engine/physics/collider.h"
#include "engine/physics/convex_hull.h"

std::size_t primitive_stride_floats(const PrimitiveData &data) {
  if (data.hasSkin) {
    return 16U;
  }
  return data.hasUVs ? 8U : 6U;
}

void apply_scale_to_primitive(PrimitiveData *data, float scaleFactor) {
  if ((data == nullptr) || (scaleFactor == 1.0F)) {
    return;
  }

  const std::size_t strideFloats = primitive_stride_floats(*data);
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
                       PrimitiveData *outData,
                       const std::vector<std::uint32_t> *jointRemap) {
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

  const cgltf_accessor *joints =
      find_attribute_accessor(primitive, cgltf_attribute_type_joints);
  const cgltf_accessor *weights =
      find_attribute_accessor(primitive, cgltf_attribute_type_weights);
  const bool hasSkin = (jointRemap != nullptr) && (joints != nullptr) &&
                       (weights != nullptr) &&
                       (joints->type == cgltf_type_vec4) &&
                       (weights->type == cgltf_type_vec4) &&
                       (joints->count == positions->count) &&
                       (weights->count == positions->count);
  outData->hasSkin = hasSkin;
  const std::size_t strideFloats = primitive_stride_floats(*outData);

  const std::size_t vertexCount = static_cast<std::size_t>(positions->count);
  outData->interleavedVertices.assign(vertexCount * strideFloats, 0.0F);

  std::array<float, 3U> position{};
  std::array<float, 3U> normal{};
  std::array<float, 2U> uv{};
  std::array<cgltf_uint, 4U> jointIndices{};
  std::array<float, 4U> jointWeights{};

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

    if (hasSkin) {
      if (!cgltf_accessor_read_uint(joints, static_cast<cgltf_size>(i),
                                    jointIndices.data(),
                                    jointIndices.size()) ||
          !cgltf_accessor_read_float(weights, static_cast<cgltf_size>(i),
                                     jointWeights.data(),
                                     jointWeights.size())) {
        std::fprintf(stderr, "error: failed to decode skin attributes\n");
        return false;
      }
      const float weightSum = jointWeights[0U] + jointWeights[1U] +
                              jointWeights[2U] + jointWeights[3U];
      for (std::size_t c = 0U; c < 4U; ++c) {
        const std::uint32_t joint =
            static_cast<std::uint32_t>(jointIndices[c]);
        if (joint >= jointRemap->size()) {
          std::fprintf(stderr, "error: vertex joint index out of range\n");
          return false;
        }
        outData->interleavedVertices[base + 8U + c] =
            static_cast<float>((*jointRemap)[joint]);
        outData->interleavedVertices[base + 12U + c] =
            (weightSum > 0.0F) ? (jointWeights[c] / weightSum) : 0.0F;
      }
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

/// Writes mesh file data.
bool write_mesh_file(const char *outputPath, const PrimitiveData &data) {
  if (outputPath == nullptr) {
    return false;
  }

  const std::size_t strideFloats = primitive_stride_floats(data);
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
  header.version = data.hasSkin
                       ? engine::core::kMeshAssetVersion3
                       : (data.hasUVs ? engine::core::kMeshAssetVersion2
                                      : engine::core::kMeshAssetVersion);
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

/// Writes metadata file data.
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

  const std::size_t vertexCount =
      data.interleavedVertices.size() / primitive_stride_floats(data);
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

  char sourceHashText[17] = {};
  format_hex_u64(sourceHash, sourceHashText);

  engine::core::JsonWriter writer{};
  writer.begin_object();
  writer.write_uint("schemaVersion", 2U);
  writer.write_string("assetId", sourceHashText);
  writer.write_string("typeTag", "mesh");
  writer.write_string("source", inputPath);
  writer.write_string("output", outputPath);
  writer.write_string("assetFormat", "engine.mesh");
  writer.write_uint("assetFormatVersion",
                    data.hasSkin
                        ? engine::core::kMeshAssetVersion3
                        : (data.hasUVs ? engine::core::kMeshAssetVersion2
                                       : engine::core::kMeshAssetVersion));
  writer.write_string("sourceContentHash", sourceHashText);
  writer.write_uint64("fileSize", outputFileSize);
  writer.write_uint64("vertexCount", static_cast<std::uint64_t>(vertexCount));
  writer.write_uint64("indexCount", static_cast<std::uint64_t>(indexCount));
  writer.begin_array("tags");
  writer.end_array();
  writer.begin_array("dependencies");
  for (const DependencyDigest &dependency : dependencies) {
    char dependencyHashText[17] = {};
    format_hex_u64(dependency.hash, dependencyHashText);
    writer.begin_object();
    writer.write_string("path", dependency.path.c_str());
    writer.write_string("hash", dependencyHashText);
    writer.end_object();
  }
  writer.end_array();

  writer.write_key("importSettings");
  writer.begin_object();
  writer.write_uint64("meshIndex",
                      static_cast<std::uint64_t>(importSettings.meshIndex));
  writer.write_uint64("primitiveIndex", static_cast<std::uint64_t>(
                                            importSettings.primitiveIndex));
  writer.write_float("scaleFactor", importSettings.scaleFactor);
  writer.write_uint64("upAxis",
                      static_cast<std::uint64_t>(importSettings.upAxis));
  writer.write_bool("generateNormals", importSettings.generateNormals);
  writer.write_string("interleavedLayout", data.hasUVs
                                               ? "position_normal_texcoord"
                                               : "position_normal");
  writer.end_object();
  writer.end_object();

  return writer.ok() &&
         write_text_file(metadataPath, writer.result(), writer.result_size());
}

bool cook_and_write_convex_hull(const char *outputPath,
                                const PrimitiveData &data) {
  if (outputPath == nullptr) {
    return false;
  }

  const std::size_t strideFloats = primitive_stride_floats(data);
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

/// Extracts validated glTF dependencies and cookstamp digests.
bool extract_gltf_dependencies(const cgltf_data *data, const char *inputPath,
                               std::uint64_t meshAssetId,
                               engine::tools::DependencyGraph *graph,
                               std::vector<DependencyDigest> *autoDepDigests) {
  if ((data == nullptr) || (inputPath == nullptr) || (graph == nullptr)) {
    return false;
  }

  // Walk all materials used by this mesh's primitives.
  std::unordered_set<const cgltf_image *> seenImages{};
  bool graphValid = true;

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
    if (!engine::tools::add_dependency(graph, meshAssetId, texAssetId)) {
      graphValid = false;
      return;
    }

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
        if (!engine::tools::add_dependency(graph, meshAssetId, matAssetId)) {
          graphValid = false;
        }
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
  return graphValid;
}

