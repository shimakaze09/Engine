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

/// One manifest-listed cooked output with the content hash recorded when
/// the stamp committed the cook (issue #55).
struct OutputRecord final {
  std::string path{};
  std::uint64_t hash = 0ULL;
};

/// Importer contract version baked into every cook stamp: bump whenever
/// the cooked output format or import semantics change, so existing
/// outputs recook once instead of silently keeping stale bytes (audit
/// H-20). Stamps written before this key existed read as version 0 and
/// therefore always recook. Version 3 introduced the output manifest
/// (issue #55); pre-manifest stamps recook once through this gate.
inline constexpr std::uint32_t kCookToolVersion = 3U;

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
/// Writes the cook stamp recording source/settings hashes, dependency
/// digests, and the output manifest hashed from the committed files;
/// an unreadable listed output fails the write so the stamp can never
/// certify an output set it could not fingerprint (issue #55).
bool write_cook_stamp(const char *outputPath, std::uint64_t sourceHash,
                      const std::vector<DependencyDigest> &dependencies,
                      std::uint64_t importSettingsHash,
                      const std::vector<std::string> &outputPaths);
/// True when the output must be recooked: stamp missing, legacy, or
/// stale hashes, a manifest-listed output missing, or (with
/// verifyOutputHashes) a manifest-listed output whose bytes changed.
bool should_repack(const char *outputPath, std::uint64_t sourceHash,
                   const std::vector<DependencyDigest> &dependencies,
                   std::uint64_t importSettingsHash,
                   bool verifyOutputHashes = false);
/// Deletes previous-manifest outputs the current cook no longer
/// produces (renamed/removed clips, hull-less recooks); a failed
/// deletion returns false and must block the new stamp (issue #55).
bool remove_stale_outputs(const char *outputPath,
                          const std::vector<std::string> &currentOutputs);

/// Rotates positions and normals from the declared source up axis
/// (0 = X-up, 2 = Z-up) into engine Y-up; 1 and unknown values no-op.
/// Proper rotations only, so triangle winding is preserved (audit H-20:
/// the setting was hashed but never applied).
void apply_up_axis_to_primitive(PrimitiveData *data, int upAxis);
/// Recomputes per-vertex normals as area-weighted face-normal averages
/// over the primitive's triangles (audit H-20: the setting was hashed
/// but never applied).
void generate_normals_for_primitive(PrimitiveData *data);
/// Uniform scale applied in place to an extracted primitive.
void apply_scale_to_primitive(PrimitiveData *data, float scaleFactor);
/// Extracts one glTF primitive into interleaved vertices and indices.
/// When jointRemap is non-null and the primitive carries JOINTS_0 and
/// WEIGHTS_0, cooks the skinned v3 layout with joint indices remapped to
/// the reordered skeleton. allowMissingNormals accepts sources without a
/// NORMAL accessor, leaving zeroed normals for the caller to generate.
bool extract_primitive(const cgltf_primitive *primitive,
                       PrimitiveData *outData,
                       const std::vector<std::uint32_t> *jointRemap = nullptr,
                       bool allowMissingNormals = false);
/// Writes the cooked .mesh file.
bool write_mesh_file(const char *outputPath, const PrimitiveData &data);
/// Writes the .meta.json metadata sidecar.
bool write_metadata_file(const char *inputPath, const char *outputPath,
                         const PrimitiveData &data,
                         std::uint64_t sourceHash,
                         const std::vector<DependencyDigest> &dependencies,
                         const ImportSettings &importSettings);
/// Cooks and writes the collision convex hull beside the mesh. True when
/// the sidecar was written OR the geometry structurally has no hull
/// (too few vertices, degenerate); false only on a write failure, which
/// must block the cook-stamp commit marker.
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
/// Checksum sidecar path for a thumbnail (".../foo.png" ->
/// ".../foo.checksum"), shared so the cook manifest can list it.
void build_thumbnail_checksum_path(const char *thumbPath, char *checksumPath,
                                   std::size_t size) noexcept;
/// Renders/copies a texture asset thumbnail; skipped when up to date.
bool generate_texture_thumbnail(const char *inputPath,
                                const char *outputPath) noexcept;
/// Rasterizes a mesh thumbnail; skipped only when both the source bytes
/// and the import-settings hash match the stored sidecar, so a settings
/// change (scale, mesh/primitive index) regenerates it (audit M-28).
bool generate_mesh_thumbnail(const char *inputPath, const char *outputPath,
                             const PrimitiveData &data,
                             std::uint64_t importSettingsHash);
