// Implements the glTF mesh importer: a .gltf or .glb source cooks to one
// .mesh (the primitive its import settings select) with its .cookmeta,
// convex hull and thumbnail, plus a .skel and one .anim per clip when the
// source is skinned.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cgltf.h>

#include "anim_cook.h"
#include "animation_import.h"
#include "dependency_graph.h"
#include "importer.h"
#include "packer_shared.h"
#include "skeleton_import.h"

namespace engine::tools {
namespace {

/// A .mesh holds one primitive, so a cook takes one mesh's one primitive
/// and nothing else of the file. Says so, naming what it leaves behind
/// and how to choose another, and says when a requested index is out of
/// range and the first is cooked instead, so no part of a model goes
/// missing without a word -- Unreal's importer reports what it skipped
/// in the same way. At most kListed parts are named; the rest are
/// counted.
void report_uncooked_primitives(const cgltf_data &data,
                                const ImportSettings &requested,
                                cgltf_size meshIdx, cgltf_size primIdx,
                                const char *inputPath) noexcept {
  if (static_cast<cgltf_size>(requested.meshIndex) != meshIdx) {
    std::fprintf(stderr,
                 "warning: importSettings.meshIndex %d is out of range (%s "
                 "holds %zu meshes); mesh %zu is cooked\n",
                 requested.meshIndex, inputPath,
                 static_cast<std::size_t>(data.meshes_count),
                 static_cast<std::size_t>(meshIdx));
  }
  if ((static_cast<cgltf_size>(requested.meshIndex) == meshIdx) &&
      (static_cast<cgltf_size>(requested.primitiveIndex) != primIdx)) {
    std::fprintf(
        stderr,
        "warning: importSettings.primitiveIndex %d is out of range "
        "(mesh %zu holds %zu primitives); primitive %zu is cooked\n",
        requested.primitiveIndex, static_cast<std::size_t>(meshIdx),
        static_cast<std::size_t>(data.meshes[meshIdx].primitives_count),
        static_cast<std::size_t>(primIdx));
  }

  std::size_t total = 0U;
  for (cgltf_size m = 0U; m < data.meshes_count; ++m) {
    total += static_cast<std::size_t>(data.meshes[m].primitives_count);
  }
  if (total <= 1U) {
    return;
  }
  std::fprintf(stderr,
               "warning: %s holds %zu meshes with %zu primitives; a .mesh "
               "holds one, so mesh %zu primitive %zu is cooked and the other "
               "%zu are not imported (choose one with importSettings.meshIndex "
               "and primitiveIndex in %s.meta):\n",
               inputPath, static_cast<std::size_t>(data.meshes_count), total,
               static_cast<std::size_t>(meshIdx),
               static_cast<std::size_t>(primIdx), total - 1U, inputPath);
  constexpr std::size_t kListed = 16U;
  std::size_t listed = 0U;
  for (cgltf_size m = 0U; m < data.meshes_count; ++m) {
    const cgltf_mesh &mesh = data.meshes[m];
    for (cgltf_size p = 0U; p < mesh.primitives_count; ++p) {
      if ((m == meshIdx) && (p == primIdx)) {
        continue;
      }
      if (listed == kListed) {
        std::fprintf(stderr, "  ... and %zu more\n", total - 1U - kListed);
        return;
      }
      const cgltf_material *material = mesh.primitives[p].material;
      std::fprintf(
          stderr,
          "  not imported: mesh %zu '%s' primitive %zu (material '%s')\n",
          static_cast<std::size_t>(m), (mesh.name != nullptr) ? mesh.name : "",
          static_cast<std::size_t>(p),
          ((material != nullptr) && (material->name != nullptr))
              ? material->name
              : "");
      ++listed;
    }
  }
}

/// Human-readable name for a cgltf result in a diagnostic; a fixed
/// placeholder for any result the enumeration adds later.
const char *cgltf_result_name(cgltf_result result) {
  switch (result) {
  case cgltf_result_success:
    return "success";
  case cgltf_result_data_too_short:
    return "data too short";
  case cgltf_result_unknown_format:
    return "unknown format";
  case cgltf_result_invalid_json:
    return "invalid json";
  case cgltf_result_invalid_gltf:
    return "invalid gltf";
  case cgltf_result_invalid_options:
    return "invalid options";
  case cgltf_result_file_not_found:
    return "file not found";
  case cgltf_result_io_error:
    return "io error";
  case cgltf_result_out_of_memory:
    return "out of memory";
  case cgltf_result_legacy_gltf:
    return "legacy gltf";
  default:
    return "unrecognized result";
  }
}

/// Strips the mesh output's extension so cooked skeletal assets land
/// beside it ("chars/hero.mesh" -> "chars/hero").
std::string cooked_output_base(const char *outputPath) {
  std::string base(outputPath);
  const std::size_t separator = base.find_last_of("/\\");
  const std::size_t dot = base.rfind('.');
  if ((dot != std::string::npos) &&
      ((separator == std::string::npos) || (dot > separator))) {
    base.resize(dot);
  }
  return base;
}

/// Cooks skin 0 and every animation into "<base>.skel" and
/// "<base>.<clip>.anim" beside the mesh output, filling outJointRemap for
/// skinned vertex extraction and appending every committed path to
/// outCookedPaths for the stamp's output manifest; returns 0
/// on success or the packer exit code (14 skeleton, 15 animation).
int cook_skeletal_assets(const cgltf_data *data, const char *outputPath,
                         std::vector<std::uint32_t> *outJointRemap,
                         std::vector<std::string> *outCookedPaths) {
  Skeleton skeleton{};
  SkeletonImportResult skeletonResult = SkeletonImportResult::Ok;
  if (!parse_gltf_skeleton(data, 0U, &skeleton, &skeletonResult)) {
    std::fprintf(stderr, "error: failed to import glTF skin: %s\n",
                 skeleton_import_result_message(skeletonResult));
    return 14;
  }

  std::vector<std::uint32_t> &jointRemap = *outJointRemap;
  if (!reorder_skeleton_parent_first(&skeleton, &jointRemap)) {
    std::fprintf(stderr, "error: skeleton parent links form a cycle\n");
    return 14;
  }

  const std::string base = cooked_output_base(outputPath);
  const std::string skeletonPath = base + ".skel";
  if (!write_skeleton_asset(skeletonPath.c_str(), skeleton)) {
    std::fprintf(stderr, "error: failed to write cooked skeleton: %s\n",
                 skeletonPath.c_str());
    return 14;
  }
  std::printf("cooked skeleton: %s (%zu joints)\n", skeletonPath.c_str(),
              skeleton.joints.size());
  outCookedPaths->push_back(skeletonPath);

  std::unordered_set<std::string> usedClipNames{};
  for (std::size_t animIndex = 0U; animIndex < data->animations_count;
       ++animIndex) {
    AnimClip clip{};
    AnimationImportResult animationResult = AnimationImportResult::Ok;
    if (!parse_gltf_animation(data, animIndex, 0U, &clip, &animationResult)) {
      std::fprintf(stderr, "error: failed to import glTF animation %zu: %s\n",
                   animIndex, animation_import_result_message(animationResult));
      return 15;
    }
    std::string clipName{};
    if (!derive_unique_clip_name(clip.name, animIndex, &usedClipNames,
                                 &clipName)) {
      std::fprintf(stderr,
                   "error: animation %zu (\"%s\") sanitizes to \"%s\", "
                   "colliding with an earlier clip's cooked output name\n",
                   animIndex, clip.name.c_str(), clipName.c_str());
      return 15;
    }
    const std::string clipPath = base + "." + clipName + ".anim";
    if (!write_anim_clip_asset(clipPath.c_str(), clip, jointRemap)) {
      std::fprintf(stderr, "error: failed to write cooked animation: %s\n",
                   clipPath.c_str());
      return 15;
    }
    std::printf("cooked animation: %s (%zu tracks, %.3fs)\n", clipPath.c_str(),
                clip.tracks.size(), static_cast<double>(clip.durationSeconds));
    outCookedPaths->push_back(clipPath);
  }
  return 0;
}

/// Records a failed cook's exit code; the cook has explained it on stderr.
CookResult failed(CookResult *result, int exitCode) {
  result->exitCode = exitCode;
  return std::move(*result);
}

std::uint64_t
hash_mesh_settings(const content::ResolvedImportSettings &settings) {
  return hash_import_settings(settings.mesh);
}

/// A glTF's external buffers and images, so a change to one recooks: the
/// stamp's dependency lines are where the relationship persists. A source
/// that will not parse adds nothing; its cook reports the failure.
void discover_gltf_dependencies(
    const char *inputPath, std::uint64_t assetId,
    std::vector<DependencyDigest> *outDependencies) {
  if (assetId == 0ULL) {
    return;
  }
  cgltf_options options{};
  cgltf_data *data = nullptr;
  if ((cgltf_parse_file(&options, inputPath, &data) == cgltf_result_success) &&
      (data != nullptr)) {
    DependencyGraph graph{};
    std::vector<DependencyDigest> discovered{};
    static_cast<void>(extract_gltf_dependencies(data, inputPath, assetId,
                                                &graph, &discovered));
    for (const DependencyDigest &dependency : discovered) {
      bool tracked = false;
      for (const DependencyDigest &existing : *outDependencies) {
        tracked = tracked || (existing.path == dependency.path);
      }
      if (!tracked) {
        outDependencies->push_back(dependency);
      }
    }
  }
  if (data != nullptr) {
    cgltf_free(data);
  }
}

CookResult cook_gltf(const CookRequest &request) {
  const char *inputPath = request.inputPath;
  const char *outputPath = request.outputPath;
  const ImportSettings &importSettings = request.settings->mesh;
  const std::vector<DependencyDigest> &dependencyDigests =
      *request.dependencies;
  const std::uint64_t sourceHash = request.sourceHash;
  const std::uint64_t importSettingsHash = request.settingsKey;
  CookResult result{};
  std::vector<std::string> &cookedOutputs = result.outputs;

  cgltf_options options{};
  cgltf_data *data = nullptr;
  const cgltf_result parseResult = cgltf_parse_file(&options, inputPath, &data);
  if ((parseResult != cgltf_result_success) || (data == nullptr)) {
    std::fprintf(stderr, "error: failed to parse glTF file: %s\n", inputPath);
    return failed(&result, 2);
  }

  const cgltf_result loadResult = cgltf_load_buffers(&options, data, inputPath);
  if (loadResult != cgltf_result_success) {
    std::fprintf(stderr, "error: failed to load glTF buffers\n");
    cgltf_free(data);
    return failed(&result, 3);
  }

  // cgltf's accessor reads trust the document's counts and offsets; only
  // validation checks them against the loaded buffer sizes, so without it
  // an accessor that overruns its view cooks bytes from beyond the buffer.
  const cgltf_result validateResult = cgltf_validate(data);
  if (validateResult != cgltf_result_success) {
    std::fprintf(stderr, "error: glTF failed validation (%s): %s\n",
                 cgltf_result_name(validateResult), inputPath);
    cgltf_free(data);
    return failed(&result, 7);
  }

  if ((data->meshes_count == 0U) || (data->meshes[0].primitives_count == 0U) ||
      (data->meshes[0].primitives == nullptr)) {
    std::fprintf(stderr, "error: glTF has no mesh primitives\n");
    cgltf_free(data);
    return failed(&result, 4);
  }

  std::vector<std::uint32_t> jointRemap{};
  if (data->skins_count > 0U) {
    const int skeletalExitCode =
        cook_skeletal_assets(data, outputPath, &jointRemap, &cookedOutputs);
    if (skeletalExitCode != 0) {
      cgltf_free(data);
      return failed(&result, skeletalExitCode);
    }
  }

  const cgltf_size meshIdx =
      (importSettings.meshIndex >= 0 &&
       static_cast<cgltf_size>(importSettings.meshIndex) < data->meshes_count)
          ? static_cast<cgltf_size>(importSettings.meshIndex)
          : 0U;
  const cgltf_mesh &selectedMesh = data->meshes[meshIdx];
  // Only mesh 0 is validated at load; a meta-selected mesh needs its own
  // primitive check or primitives[0] below indexes an empty array.
  if (selectedMesh.primitives_count == 0U) {
    std::fprintf(stderr,
                 "error: selected mesh %zu has no primitives "
                 "(importSettings.meshIndex in %s.meta)\n",
                 static_cast<std::size_t>(meshIdx), inputPath);
    cgltf_free(data);
    return failed(&result, 5);
  }
  const cgltf_size primIdx =
      (importSettings.primitiveIndex >= 0 &&
       static_cast<cgltf_size>(importSettings.primitiveIndex) <
           selectedMesh.primitives_count)
          ? static_cast<cgltf_size>(importSettings.primitiveIndex)
          : 0U;

  report_uncooked_primitives(*data, importSettings, meshIdx, primIdx,
                             inputPath);

  // What was cooked, which the sidecar records: an index out of range
  // falls back to the first, and the sidecar must not claim the request.
  ImportSettings cookedSettings = importSettings;
  cookedSettings.meshIndex = static_cast<std::int32_t>(meshIdx);
  cookedSettings.primitiveIndex = static_cast<std::int32_t>(primIdx);

  const cgltf_primitive *primitive = &selectedMesh.primitives[primIdx];
  PrimitiveData primitiveData{};
  if (!extract_primitive(primitive, &primitiveData,
                         jointRemap.empty() ? nullptr : &jointRemap,
                         importSettings.generateNormals)) {
    cgltf_free(data);
    return failed(&result, 5);
  }

  if (primitiveData.hasSkin && (importSettings.upAxis != 1)) {
    // The skeleton's inverse binds are not rotated with the mesh, so an
    // axis conversion would desync the two; reject rather than desync.
    std::fprintf(stderr, "error: upAxis conversion is unsupported for skinned "
                         "meshes — re-export the source Y-up\n");
    cgltf_free(data);
    return failed(&result, 5);
  }
  if ((importSettings.upAxis < 0) || (importSettings.upAxis > 2)) {
    std::fprintf(stderr, "warning: unknown upAxis %d ignored (treated Y-up)\n",
                 importSettings.upAxis);
  }
  apply_up_axis_to_primitive(&primitiveData, importSettings.upAxis);
  if (importSettings.generateNormals) {
    generate_normals_for_primitive(&primitiveData);
  }
  apply_scale_to_primitive(&primitiveData, importSettings.scaleFactor);

  const bool writeOk = write_mesh_file(outputPath, primitiveData);
  cgltf_free(data);

  if (!writeOk) {
    return failed(&result, 6);
  }
  cookedOutputs.emplace_back(outputPath);

  if (!write_metadata_file(inputPath, outputPath, primitiveData, sourceHash,
                           dependencyDigests, cookedSettings)) {
    std::fprintf(stderr, "error: failed to write metadata sidecar\n");
    return failed(&result, 12);
  }
  cookedOutputs.push_back(std::string(outputPath) + ".cookmeta");

  // Hull-less geometry reports success; only a write failure blocks the
  // stamp below so a broken sidecar can never be certified complete.
  if (!cook_and_write_convex_hull(outputPath, primitiveData)) {
    std::fprintf(stderr, "error: failed to write convex hull sidecar\n");
    return failed(&result, 17);
  }
  const std::string hullPath = std::string(outputPath) + ".hull";
  if (file_exists(hullPath.c_str())) {
    cookedOutputs.push_back(hullPath);
  }

  // The builders are fallible — an overlong destination refuses
  // instead of redirecting into the working directory. With no buildable
  // path there is nothing to retire or certify.
  char thumbPath[512] = {};
  char thumbChecksumPath[512] = {};
  const bool thumbPathsOk =
      build_thumbnail_path(outputPath, thumbPath, sizeof(thumbPath)) &&
      build_thumbnail_checksum_path(thumbPath, thumbChecksumPath,
                                    sizeof(thumbChecksumPath));
  if (!thumbPathsOk ||
      !generate_mesh_thumbnail(inputPath, outputPath, primitiveData,
                               importSettingsHash)) {
    std::fprintf(stderr, "warning: mesh thumbnail generation failed: %s\n",
                 outputPath);
    // A failed regeneration must not leave the previous generation's
    // thumbnail behind for the fresh stamp below to certify as current; a
    // stale file that cannot be retired blocks the stamp entirely.
    if (thumbPathsOk && !retire_stale_thumbnail(thumbPath, thumbChecksumPath)) {
      return failed(&result, 20);
    }
  }
  if (thumbPathsOk && file_exists(thumbPath)) {
    cookedOutputs.emplace_back(thumbPath);
    if (file_exists(thumbChecksumPath)) {
      cookedOutputs.emplace_back(thumbChecksumPath);
    }
  }

  char summary[256] = {};
  std::snprintf(summary, sizeof(summary),
                "packed mesh: vertices=%zu indices=%zu uvs=%s skin=%s -> %s "
                "(+ .cookmeta)",
                primitiveData.interleavedVertices.size() /
                    primitive_stride_floats(primitiveData),
                primitiveData.indices.size(),
                primitiveData.hasUVs ? "yes" : "no",
                primitiveData.hasSkin ? "yes" : "no", outputPath);
  result.summary = summary;
  return result;
}

} // namespace

const Importer kGltfMeshImporter{"glTF mesh",
                                 content::AssetTypeTag::Mesh,
                                 kMeshCookLogicRevision,
                                 &hash_mesh_settings,
                                 &discover_gltf_dependencies,
                                 &cook_gltf,
                                 true};

} // namespace engine::tools
