// Implements main behavior for the Engine tooling.

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

#include <cgltf.h>

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // sprintf deprecated
#pragma warning(disable : 4244) // stb_image: int->short conversion
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
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

#include "anim_cook.h"
#include "animation_import.h"
#include "dependency_graph.h"
#include "skeleton_import.h"
#include "thumbnail_resample.h"

#include "packer_shared.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "usage: asset_packer <input.gltf|input.glb> <output.mesh> "
               "[--dep <dependency_path>]... [--graph <asset_deps.json>] "
               "[--force]\n");
}

/// Replaces every character outside [A-Za-z0-9_-] so clip names cook to
/// portable file names; empty names fall back to "clip<index>".
std::string sanitize_clip_name(const std::string &name, std::size_t index) {
  std::string cleaned{};
  cleaned.reserve(name.size());
  for (const char c : name) {
    const bool keep = ((c >= 'a') && (c <= 'z')) ||
                      ((c >= 'A') && (c <= 'Z')) ||
                      ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-');
    cleaned.push_back(keep ? c : '_');
  }
  if (cleaned.empty()) {
    cleaned = "clip" + std::to_string(index);
  }
  return cleaned;
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
/// skinned vertex extraction; returns 0 on success or the packer exit
/// code (14 skeleton, 15 animation).
int cook_skeletal_assets(const cgltf_data *data, const char *outputPath,
                         std::vector<std::uint32_t> *outJointRemap) {
  engine::tools::Skeleton skeleton{};
  engine::tools::SkeletonImportResult skeletonResult =
      engine::tools::SkeletonImportResult::Ok;
  if (!engine::tools::parse_gltf_skeleton(data, 0U, &skeleton,
                                          &skeletonResult)) {
    std::fprintf(stderr, "error: failed to import glTF skin: %s\n",
                 engine::tools::skeleton_import_result_message(skeletonResult));
    return 14;
  }

  std::vector<std::uint32_t> &jointRemap = *outJointRemap;
  if (!engine::tools::reorder_skeleton_parent_first(&skeleton, &jointRemap)) {
    std::fprintf(stderr, "error: skeleton parent links form a cycle\n");
    return 14;
  }

  const std::string base = cooked_output_base(outputPath);
  const std::string skeletonPath = base + ".skel";
  if (!engine::tools::write_skeleton_asset(skeletonPath.c_str(), skeleton)) {
    std::fprintf(stderr, "error: failed to write cooked skeleton: %s\n",
                 skeletonPath.c_str());
    return 14;
  }
  std::printf("cooked skeleton: %s (%zu joints)\n", skeletonPath.c_str(),
              skeleton.joints.size());

  for (std::size_t animIndex = 0U; animIndex < data->animations_count;
       ++animIndex) {
    engine::tools::AnimClip clip{};
    engine::tools::AnimationImportResult animationResult =
        engine::tools::AnimationImportResult::Ok;
    if (!engine::tools::parse_gltf_animation(data, animIndex, 0U, &clip,
                                             &animationResult)) {
      std::fprintf(
          stderr, "error: failed to import glTF animation %zu: %s\n",
          animIndex,
          engine::tools::animation_import_result_message(animationResult));
      return 15;
    }
    const std::string clipPath =
        base + "." + sanitize_clip_name(clip.name, animIndex) + ".anim";
    if (!engine::tools::write_anim_clip_asset(clipPath.c_str(), clip,
                                              jointRemap)) {
      std::fprintf(stderr, "error: failed to write cooked animation: %s\n",
                   clipPath.c_str());
      return 15;
    }
    std::printf("cooked animation: %s (%zu tracks, %.3fs)\n",
                clipPath.c_str(), clip.tracks.size(),
                static_cast<double>(clip.durationSeconds));
  }
  return 0;
}

} // namespace

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

// ---------------------------------------------------------------------------
// Thumbnail checksum helpers
// ---------------------------------------------------------------------------

// Build checksum sidecar path from thumbnail PNG path.
// E.g. ".thumbnails/foo.png" -> ".thumbnails/foo.checksum"


/// Runs this executable or test program.
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

  engine::tools::DependencyGraph depGraph{};
  const bool hasGraphPath = !graphPath.empty();
  if (hasGraphPath && file_exists(graphPath.c_str()) &&
      !engine::tools::read_dependency_graph_json(&depGraph,
                                                 graphPath.c_str())) {
    std::fprintf(stderr, "error: failed to read dependency graph: %s\n",
                 graphPath.c_str());
    return 15;
  }

  const std::uint64_t meshAssetId = hash_path_to_asset_id(inputPath);
  if (hasGraphPath && (meshAssetId != 0ULL)) {
    engine::tools::register_asset_path(&depGraph, meshAssetId, inputPath);

    // Graph-tracked dependencies (auto-discovered on earlier cooks) can
    // force a repack beyond the explicit --dep flags.
    engine::tools::DependencyGraph::AssetId depIds[64] = {};
    const std::size_t depCount =
        engine::tools::get_dependencies(&depGraph, meshAssetId, depIds, 64U);
    for (std::size_t i = 0U; i < depCount; ++i) {
      auto pathIt = depGraph.assetPaths.find(depIds[i]);
      if (pathIt != depGraph.assetPaths.end()) {
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

  {
    const char *ext = std::strrchr(inputPath, '.');
    if (ext != nullptr) {
      const bool isPng =
          std::strcmp(ext, ".png") == 0 || std::strcmp(ext, ".PNG") == 0;
      const bool isJpg =
          std::strcmp(ext, ".jpg") == 0 || std::strcmp(ext, ".jpeg") == 0 ||
          std::strcmp(ext, ".JPG") == 0 || std::strcmp(ext, ".JPEG") == 0;
      if (isPng || isJpg) {
            return generate_texture_thumbnail(inputPath, outputPath) ? 0 : 14;
      }
    }
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

  std::vector<std::uint32_t> jointRemap{};
  if (data->skins_count > 0U) {
    const int skeletalExitCode =
        cook_skeletal_assets(data, outputPath, &jointRemap);
    if (skeletalExitCode != 0) {
      cgltf_free(data);
      return skeletalExitCode;
    }
  }

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
  if (!extract_primitive(primitive, &primitiveData,
                         jointRemap.empty() ? nullptr : &jointRemap)) {
    cgltf_free(data);
    return 5;
  }

  apply_scale_to_primitive(&primitiveData, importSettings.scaleFactor);

  std::vector<DependencyDigest> autoDiscoveredDeps{};
  if (hasGraphPath && (meshAssetId != 0ULL)) {
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

    if (!extract_gltf_dependencies(data, inputPath, meshAssetId, &depGraph,
                                   &autoDiscoveredDeps)) {
      std::fprintf(stderr,
                   "error: glTF dependencies would make the graph invalid\n");
      cgltf_free(data);
      return 15;
    }

      for (const auto &manualDep : dependencyDigests) {
      const std::uint64_t depId = hash_path_to_asset_id(manualDep.path.c_str());
      if (depId != 0ULL) {
        engine::tools::register_asset_path(&depGraph, depId,
                                           manualDep.path.c_str());
        if (!engine::tools::add_dependency(&depGraph, meshAssetId, depId)) {
          std::fprintf(stderr,
                       "error: dependency would make the graph invalid: %s\n",
                       manualDep.path.c_str());
          cgltf_free(data);
          return 15;
        }
      }
    }
  }

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

  cook_and_write_convex_hull(outputPath, primitiveData);

  generate_mesh_thumbnail(inputPath, outputPath, primitiveData);

  if (hasGraphPath) {
    if (!engine::tools::write_dependency_graph_json(&depGraph,
                                                    graphPath.c_str())) {
      std::fprintf(stderr, "error: failed to write dependency graph: %s\n",
                   graphPath.c_str());
      return 16;
    }
  }

  std::printf(
      "packed mesh: vertices=%zu indices=%zu uvs=%s skin=%s -> %s "
      "(+ .meta.json)\n",
      primitiveData.interleavedVertices.size() /
          primitive_stride_floats(primitiveData),
      primitiveData.indices.size(), primitiveData.hasUVs ? "yes" : "no",
      primitiveData.hasSkin ? "yes" : "no", outputPath);
  return 0;
}
