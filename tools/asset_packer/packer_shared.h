// Declares the asset packer's shared cook surface: primitive payloads,
// dependency digests, import settings, incremental cook-stamp bookkeeping,
// glTF mesh extraction/writing, and thumbnail generation.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <cgltf.h>

#include "dependency_graph.h"

/// Interleaved vertex/index payload extracted from one glTF primitive.
struct PrimitiveData final {
  std::vector<float> interleavedVertices{};
  std::vector<std::uint32_t> indices{};
  bool hasUVs = false;
  bool hasSkin = false;
};

/// Interleaved floats per vertex for a primitive's layout: 16 skinned
/// (uv slot always present), 8 with UVs, 6 bare.
std::size_t primitive_stride_floats(const PrimitiveData &data);

/// One dependency path with its content hash, for cook-stamp comparison.
struct DependencyDigest final {
  std::string path{};
  std::uint64_t hash = 0ULL;
};

/// Import settings read from an asset's .meta.json sidecar.
struct ImportSettings final {
  int meshIndex = 0;
  int primitiveIndex = 0;
  float scaleFactor = 1.0F;
  int upAxis = 1;
  bool generateNormals = false;
};

/// Whether the file exists and is readable.
bool file_exists(const char *path);
/// Writes a text buffer to a file; false on IO failure.
bool write_text_file(const char *path, const char *text, std::size_t textSize);
/// Formats a 64-bit value as 16 lowercase hex digits.
void format_hex_u64(std::uint64_t value, char (&out)[17]) noexcept;
/// FNV-1a hash of the file's bytes; ok reports read success.
std::uint64_t hash_file_contents(const char *path, bool *ok);
/// Creates the directory (and parents) when missing.
bool ensure_directory_exists(const char *dirPath);
/// Deterministic 64-bit asset id from a canonicalized path.
std::uint64_t hash_path_to_asset_id(const char *path);

/// Hashes every dependency's content into sorted digests.
bool build_dependency_digests(const std::vector<std::string> &dependencyPaths,
                              std::vector<DependencyDigest> *outDigests);
/// Order-independent hash of the import settings block.
std::uint64_t hash_import_settings(const ImportSettings &settings);
/// Sorts digests by path for deterministic stamp layout.
void sort_dependency_digests(std::vector<DependencyDigest> &digests);
/// Reads import settings from the output's .meta.json when present.
bool read_import_settings_from_meta(const char *outputPath,
                                    ImportSettings *outSettings);
/// Writes the cook stamp recording source/settings hashes and digests.
bool write_cook_stamp(const char *outputPath, std::uint64_t sourceHash,
                      const std::vector<DependencyDigest> &dependencies,
                      std::uint64_t importSettingsHash);
/// True when the output must be recooked (stamp missing or stale).
bool should_repack(const char *outputPath, std::uint64_t sourceHash,
                   const std::vector<DependencyDigest> &dependencies,
                   std::uint64_t importSettingsHash);

/// Uniform scale applied in place to an extracted primitive.
void apply_scale_to_primitive(PrimitiveData *data, float scaleFactor);
/// Extracts one glTF primitive into interleaved vertices and indices.
/// When jointRemap is non-null and the primitive carries JOINTS_0 and
/// WEIGHTS_0, cooks the skinned v3 layout with joint indices remapped to
/// the reordered skeleton.
bool extract_primitive(const cgltf_primitive *primitive,
                       PrimitiveData *outData,
                       const std::vector<std::uint32_t> *jointRemap = nullptr);
/// Writes the cooked .mesh file.
bool write_mesh_file(const char *outputPath, const PrimitiveData &data);
/// Writes the .meta.json metadata sidecar.
bool write_metadata_file(const char *inputPath, const char *outputPath,
                         const PrimitiveData &data,
                         std::uint64_t sourceHash,
                         const std::vector<DependencyDigest> &dependencies,
                         const ImportSettings &importSettings);
/// Cooks and writes the collision convex hull beside the mesh.
bool cook_and_write_convex_hull(const char *outputPath,
                                const PrimitiveData &data);
/// Resolves a glTF image URI relative to the input file.
bool resolve_image_path(const char *inputPath, const char *imageUri,
                        char *outPath, std::size_t outPathSize);
/// Collects the glTF's external dependencies (buffers, images) into the
/// dependency graph and digest list.
bool extract_gltf_dependencies(const cgltf_data *data, const char *inputPath,
                               std::uint64_t meshAssetId,
                               engine::tools::DependencyGraph *graph,
                               std::vector<DependencyDigest> *autoDepDigests);

/// Thumbnail output path beside the cooked asset (.thumbnails/<name>.png).
void build_thumbnail_path(const char *outputPath, char *thumbPath,
                          std::size_t thumbPathSize) noexcept;
/// Renders/copies a texture asset thumbnail; skipped when up to date.
bool generate_texture_thumbnail(const char *inputPath,
                                const char *outputPath) noexcept;
/// Rasterizes a mesh thumbnail; skipped when up to date.
bool generate_mesh_thumbnail(const char *inputPath, const char *outputPath,
                             const PrimitiveData &data);
